#include "view/view.hpp"

#include "base/diagnostic.hpp"
#include "constants/view.hpp"
#include "dom/node.hpp"
#include "script/script_platform.hpp"
#include "style/values.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>

namespace weft {
    namespace {
        // A number as JS reads it back exactly.
        std::string number_text(double value) {
            char buffer[32];
            const auto r = std::to_chars(buffer, buffer + sizeof buffer, value);
            return {buffer, r.ptr};
        }

        // Number.prototype.toLocaleString() of an integer in en-US: digits grouped by three with commas.
        std::string group_digits(std::int64_t value) {
            const bool negative = value < 0;
            const auto digits = std::to_string(negative ? 0 - std::uint64_t(value) : std::uint64_t(value));
            std::string out = negative ? "-" : "";
            for (std::size_t i = 0; i < digits.size(); ++i) {
                if (i && (digits.size() - i) % 3 == 0) {
                    out += ',';
                }
                out += digits[i];
            }
            return out;
        }

        bool faulting(const std::exception& e) {
            return std::string_view(e.what()).starts_with("WEFT_VIEW_FAULTED");
        }

        bool valid_size(float width, float height, float scale) {
            return std::isfinite(width) && width > 0 && std::isfinite(height) && height > 0 && std::isfinite(scale) &&
                   scale > 0;
        }
    } // namespace

    ViewShared::ViewShared(std::shared_ptr<bridge::EngineChannel> engine) : engine_(std::move(engine)) {
    }

    SceneAnchorWrite ViewShared::write_anchors(std::span<const SceneAnchor> anchors) {
        const std::lock_guard lock(writer_);
        return anchors_->write(anchors);
    }

    std::shared_ptr<const ViewFrame> ViewShared::frame() const {
        const std::lock_guard lock(frame_mutex_);
        return frame_;
    }

    void ViewShared::publish(std::shared_ptr<const ViewFrame> frame) {
        const std::lock_guard lock(frame_mutex_);
        frame_ = std::move(frame);
    }

    struct View::Impl {
        View& view;
        std::shared_ptr<const FontCatalog> fonts;
        SceneText& text;
        ScriptDom vm{{constants::view::vm_stack_bytes, constants::view::execution_budget_ms}};
        std::unique_ptr<SceneSolver> scene;
        // The host's reader of the files the page names, and the page load's images it read.
        ViewResourceReader reader;
        std::unique_ptr<ViewImages> images;
        SceneGeometry geometry;
        SceneFrame current;
        float width{};
        float height{};
        float scale{};
        double time{};
        bool loaded{};
        bool faulted{};
        std::string root_selector = "#root";
        NodeIdentity root_id{};
        std::uint64_t dom_revision{};
        std::optional<DomSnapshot> last_dom;
        float last_scene_width = -1;
        float last_scene_height = -1;
        double last_scene_time = -1;
        bool geometry_valid{};
        std::uint64_t layout_epoch = 1;
        double cache_time = -1;
        float cache_width = -1;
        float cache_height = -1;
        const SceneAnchorTable* anchor_table{};
        NodeIdentity hovered{};
        NodeIdentity pressed{};
        std::deque<ViewDiagnostic> diagnostics;
        std::size_t dropped{};
        // What the frame published last showed: its revision and the View's size and scale then.
        std::uint64_t published_revision{};
        std::array<float, 3> published_size{};
        bool published{};

        Impl(View& owner, std::shared_ptr<const FontCatalog> catalog, SceneText& scene_text, float w, float h, float s)
            : view(owner),
              fonts(std::move(catalog)),
              text(scene_text),
              geometry(scene_text),
              width(w),
              height(h),
              scale(s) {
        }

        void diagnose(ViewDiagnostic::Kind kind, std::string message) {
            if (diagnostics.size() >= constants::view::max_diagnostics) {
                ++dropped;
                return;
            }
            if (message.size() > constants::view::max_diagnostic_bytes) {
                message.resize(constants::view::max_diagnostic_bytes);
            }
            diagnostics.push_back({kind, std::move(message)});
        }

        // What a forgiving page's styles dropped since the last time, as scene diagnostics.
        void take_style_warnings() {
            if (!scene) {
                return;
            }
            for (auto& warning : scene->take_warnings()) {
                diagnose(ViewDiagnostic::Kind::Scene, "WEFT_STYLE_DROPPED: " + std::move(warning));
            }
        }

        // A fault of the VM ends the page; other errors are the page's.
        void handle(const std::exception& e, ViewDiagnostic::Kind kind) {
            if (faulting(e)) {
                faulted = true;
                loaded = false;
                diagnose(kind, e.what());
                throw ViewError(std::string("WEFT_VIEW_FAULTED: ") + e.what());
            }
            diagnose(kind, e.what());
        }

        void take_anchors() {
            anchor_table = &view.shared_->anchors()->take(SceneAnchorChannel::Reader::Owner);
        }

        const SceneAnchorTable* anchors() const {
            return anchor_table && anchor_table->serial() ? anchor_table : nullptr;
        }

        // The scene's root in a document snapshot: the node of the root's id or tag.
        const DomSnapshot* root_of(const DomSnapshot& document) const {
            const auto find = [&](auto&& self, const DomSnapshot& n) -> const DomSnapshot* {
                if (root_selector.starts_with('#')
                        ? n.attributes.contains("id") && n.attributes.at("id") == root_selector.substr(1)
                        : n.local_name == root_selector) {
                    return &n;
                }
                for (const auto& c : n.children) {
                    if (const auto* r = self(self, c)) {
                        return r;
                    }
                }
                return nullptr;
            };
            const auto* found = find(find, document);
            if (!found) {
                throw std::runtime_error("WEFT_VIEW_ROOT: no element " + root_selector);
            }
            return found;
        }

        // Builds the scene from last_dom; changed tells whether its content moved since the previous build.
        void sync_scene(bool changed) {
            const bool resized = width != last_scene_width || height != last_scene_height;
            if (!changed && !resized && (!current.animated || last_scene_time == time)) {
                current.time_ms = time;
                last_scene_time = time;
                return;
            }
            auto kept = geometry_valid ? std::move(current.elements) : std::vector<ElementGeometry>{};
            try {
                scene->update(*last_dom, current, width, height, time);
            } catch (...) {
                scene->reset();
                current = {};
                last_dom.reset();
                geometry_valid = false;
                throw;
            }
            const auto work = scene->work();
            geometry_valid = geometry_valid && !work.geometry_changed;
            if (geometry_valid) {
                current.elements = std::move(kept);
            }
            last_scene_width = width;
            last_scene_height = height;
            last_scene_time = time;
        }

        // A host read hands over the owner's document copy: last_dom takes only the subtrees that changed.
        void sync_read(const DomSnapshot& root) {
            bool changed = true;
            if (!last_dom) {
                last_dom = root;
            } else {
                try {
                    changed = dom::update_copy(*last_dom, root);
                } catch (...) {
                    last_dom.reset();
                    throw;
                }
            }
            sync_scene(changed);
        }

        // The scene's element geometry, which only geometry reads need: computed when one asks after a change.
        void ensure_geometry() {
            if (!geometry_valid) {
                current.elements = geometry.geometry(current, true);
                geometry_valid = true;
            }
        }

        // The DOM's changes into the scene: one snapshot of the root, refreshed in place where subtrees changed.
        void flush_scene() {
            if (!root_id.generation) {
                root_id = vm.node_identity(root_selector);
            }
            const auto revision = vm.subtree_revision(root_id);
            bool changed{};
            if (!last_dom) {
                last_dom = vm.snapshot_subtree(root_id);
                changed = true;
            } else if (revision != dom_revision) {
                try {
                    changed = vm.refresh_snapshot(root_id, *last_dom);
                } catch (...) {
                    last_dom.reset();
                    throw;
                }
            }
            dom_revision = revision;
            sync_scene(changed);
        }

        // flush_scene, a scene the styles cannot make a diagnostic (the published frame stays).
        bool try_flush() {
            try {
                flush_scene();
                return true;
            } catch (const DiagnosticError& e) {
                diagnose(ViewDiagnostic::Kind::Scene, e.what());
            } catch (const std::invalid_argument& e) {
                // The layout adapter and the text shaper report what they cannot format (an unsupported unit or
                // display, a script the shaper does not take) as invalid arguments: as the styles' errors, a scene
                // the page cannot make, which keeps its last frame, not a failure of the host's call.
                diagnose(ViewDiagnostic::Kind::Scene, e.what());
            } catch (const std::runtime_error& e) {
                handle(e, ViewDiagnostic::Kind::Scene);
            }
            return false;
        }

        void publish() {
            const std::array<float, 3> size{width, height, scale};
            if (published && current.revision && current.revision == published_revision && size == published_size) {
                return;
            }
            auto scene_copy = std::make_shared<SceneFrame>();
            scene_copy->root = current.root;
            scene_copy->animated = current.animated;
            scene_copy->time_ms = current.time_ms;
            scene_copy->revision = current.revision;
            view.shared_->publish(std::make_shared<const ViewFrame>(
                ViewFrame{std::move(scene_copy), width, height, scale, fonts, view.shared_->anchors()}));
            published_revision = current.revision;
            published_size = size;
            published = true;
        }

        void pump() {
            for (unsigned i = 0; i < constants::view::max_task_turns; ++i) {
                try {
                    if (vm.pump_tasks(time).ready == 0) {
                        return;
                    }
                } catch (const std::runtime_error& e) {
                    handle(e, ViewDiagnostic::Kind::Script);
                }
            }
            diagnose(ViewDiagnostic::Kind::Script, "WEFT_VIEW_TASKS: tasks kept scheduling tasks");
        }

        void install_hosts() {
            vm.install_integer_format_host(group_digits);
            vm.install_layout_cache_host([this] {
                if (cache_time != time || cache_width != width || cache_height != height) {
                    if (layout_epoch == std::numeric_limits<std::uint64_t>::max()) {
                        throw std::overflow_error("WEFT_VIEW_LAYOUT_EPOCH");
                    }
                    ++layout_epoch;
                    cache_time = time;
                    cache_width = width;
                    cache_height = height;
                }
                return layout_epoch;
            });
            vm.install_layout_host([this](const DomSnapshot& document) {
                sync_read(*root_of(document));
                ensure_geometry();
                if (const auto* table = anchors()) {
                    return geometry.geometry(current, true, table);
                }
                return current.elements;
            });
            vm.install_computed_style_host([this](const DomSnapshot& document, NodeIdentity identity) {
                sync_read(*root_of(document));
                const auto find = [&](auto&& self, const SceneNode& n) -> std::map<std::string, std::string> {
                    if (n.identity == identity) {
                        // getComputedStyle reads names and texts.
                        std::map<std::string, std::string> s;
                        for (const auto& [property, value] : n.style) {
                            s.emplace_hint(s.end(), std::string(property_name(property)), value.text());
                        }
                        s["width"] = number_text(n.box[2]) + "px";
                        s["height"] = number_text(n.box[3]) + "px";
                        if (!s.contains("transform") || s["transform"].empty() || s["transform"] == "none") {
                            s["transform"] = "none";
                        } else {
                            const auto m = geometry.transform_matrix(n);
                            s["transform"] = "matrix(";
                            for (unsigned i = 0; i < 6; ++i) {
                                s["transform"] += (i ? "," : "") + number_text(m[i]);
                            }
                            s["transform"] += ")";
                        }
                        return s;
                    }
                    for (const auto& c : n.children) {
                        auto s = self(self, c);
                        if (!s.empty()) {
                            return s;
                        }
                    }
                    return {};
                };
                auto s = find(find, current.root);
                if (s.empty()) {
                    s = {{"transform", "none"}, {"overflow", "visible"}, {"position", "static"}};
                }
                return s;
            });
        }

        void size_globals() {
            vm.eval("globalThis.innerWidth=" + number_text(width) + ";globalThis.innerHeight=" + number_text(height) +
                        ";globalThis.devicePixelRatio=" + number_text(scale),
                    "view");
        }

        void forget_scene() {
            current = {};
            last_dom.reset();
            root_id = {};
            dom_revision = 0;
            last_scene_width = last_scene_height = -1;
            last_scene_time = -1;
            geometry_valid = false;
            hovered = pressed = {};
            anchor_table = nullptr;
        }

        // The ancestors of id and itself in the scene, the root first; empty when the scene lacks it.
        std::vector<const SceneNode*> chain(NodeIdentity id) const {
            std::vector<const SceneNode*> path;
            const auto visit = [&](auto&& self, const SceneNode& n) -> bool {
                path.push_back(&n);
                if (n.identity == id) {
                    return true;
                }
                for (const auto& c : n.children) {
                    if (self(self, c)) {
                        return true;
                    }
                }
                path.pop_back();
                return false;
            };
            if (id.generation) {
                visit(visit, current.root);
            }
            return path;
        }
    };

    View::View(std::shared_ptr<const FontCatalog> fonts, SceneText& text, float width, float height, float scale,
               std::shared_ptr<ViewShared> shared)
        : shared_(shared ? std::move(shared)
                         : std::make_shared<ViewShared>(std::make_shared<bridge::EngineChannel>())) {
        if (!valid_size(width, height, scale)) {
            throw std::invalid_argument("WEFT_VIEW_SIZE: a positive finite size and scale");
        }
        impl_ = std::make_unique<Impl>(*this, std::move(fonts), text, width, height, scale);
        impl_->vm.install_engine(shared_->engine());
    }

    View::~View() = default;

    void View::load(const ViewPage& page) {
        auto& i = *impl_;
        i.vm.reset_document();
        i.forget_scene();
        i.loaded = false;
        i.faulted = false;
        i.root_selector = page.root.empty() ? "#root" : page.root;
        try {
            i.scene = std::make_unique<SceneSolver>(page.stylesheet, page.keyframes, i.text, true, page.forgiving_css);
        } catch (const DiagnosticError& e) {
            // A stylesheet or keyframes outside the scene profile: the page does not load, as when its script
            // throws, and the diagnostic says why.
            i.scene.reset();
            i.diagnose(ViewDiagnostic::Kind::Scene, e.what());
            throw ViewError(e.what());
        }
        // Geometry reads take the View's own (ensure_geometry).
        i.scene->set_element_geometry(false);
        i.take_style_warnings();
        // A page's images anew each load: the files may have changed.
        i.images = std::make_unique<ViewImages>(page.base, i.reader, [&i](std::string message) {
            i.diagnose(ViewDiagnostic::Kind::Scene, std::move(message));
        });
        i.scene->set_images(i.images.get());
        try {
            i.install_hosts();
            install_script_platform(i.vm, ScriptPlatformModule::WebHost);
            install_script_platform(i.vm, ScriptPlatformModule::AnimationFrame);
            install_script_platform(i.vm, ScriptPlatformModule::ScenePlatform);
            install_script_platform(i.vm, ScriptPlatformModule::Engine);
            if (page.crpg_bridge) {
                install_script_platform(i.vm, ScriptPlatformModule::CrpgBridge);
            }
            i.size_globals();
            for (const auto& script : page.scripts) {
                i.vm.eval(script.source, script.name.empty() ? "page" : script.name);
            }
        } catch (const std::runtime_error& e) {
            const bool fault = faulting(e);
            i.diagnose(ViewDiagnostic::Kind::Script, e.what());
            i.vm.reset_document();
            i.forget_scene();
            i.scene.reset();
            if (fault) {
                throw ViewError(std::string("WEFT_VIEW_FAULTED: ") + e.what());
            }
            throw ViewError(e.what());
        }
        i.loaded = true;
        advance(i.time);
    }

    void View::advance(double time_ms) {
        auto& i = *impl_;
        if (i.faulted) {
            throw ViewError("WEFT_VIEW_FAULTED: load the page again");
        }
        if (!std::isfinite(time_ms) || time_ms < i.time) {
            throw std::invalid_argument("WEFT_VIEW_TIME: a finite time no earlier than the last");
        }
        i.time = time_ms;
        if (!i.loaded) {
            return;
        }
        i.take_anchors();
        try {
            // The engine's input belongs to this frame: its handlers read the frame's time, as its tasks and animation
            // callbacks do.
            i.vm.advance_clock(i.time);
            const auto delivery = i.vm.deliver_engine();
            for (const auto& error : delivery.errors) {
                i.diagnose(ViewDiagnostic::Kind::Handler, error);
            }
        } catch (const std::runtime_error& e) {
            i.handle(e, ViewDiagnostic::Kind::Handler);
        }
        i.pump();
        try {
            const auto frame = i.vm.animation_frame(i.time);
            if (frame.exceptions) {
                i.diagnose(ViewDiagnostic::Kind::Frame, frame.exception_messages_json);
            }
        } catch (const std::runtime_error& e) {
            i.handle(e, ViewDiagnostic::Kind::Frame);
        }
        i.pump();
        if (i.try_flush()) {
            try {
                i.vm.eval("__ceScenePlatform.deliverObservers()", "view");
            } catch (const std::runtime_error& e) {
                i.handle(e, ViewDiagnostic::Kind::Script);
            }
            if (!i.try_flush()) {
                i.take_style_warnings();
                return;
            }
            i.publish();
        }
        i.take_style_warnings();
    }

    void View::resize(float width, float height, float scale) {
        auto& i = *impl_;
        if (!valid_size(width, height, scale)) {
            throw std::invalid_argument("WEFT_VIEW_SIZE: a positive finite size and scale");
        }
        if (width == i.width && height == i.height && scale == i.scale) {
            return;
        }
        i.width = width;
        i.height = height;
        i.scale = scale;
        if (i.loaded) {
            try {
                i.size_globals();
                i.vm.eval("window.dispatchEvent(new Event('resize'))", "view");
            } catch (const std::runtime_error& e) {
                i.handle(e, ViewDiagnostic::Kind::Script);
            }
        }
    }

    bool View::pointer(const PointerInput& input) {
        auto& i = *impl_;
        if (i.faulted) {
            throw ViewError("WEFT_VIEW_FAULTED: load the page again");
        }
        if (!std::isfinite(input.x) || !std::isfinite(input.y) || !std::isfinite(input.wheel_x) ||
            !std::isfinite(input.wheel_y)) {
            throw std::invalid_argument("WEFT_VIEW_POINTER: finite coordinates");
        }
        if (!i.loaded || !i.try_flush()) {
            return false;
        }
        const bool leave = input.kind == PointerKind::Leave;
        const float x = leave ? constants::view::outside : input.x;
        const float y = leave ? constants::view::outside : input.y;
        i.take_anchors();
        const auto target = leave ? NodeIdentity{} : i.geometry.hit_test(i.current, x, y, i.anchors());
        const auto destination = target.generation ? target : i.root_id;
        bool prevented{};
        unsigned buttons = input.buttons;
        try {
            const auto emit = [&](NodeIdentity identity, MouseType type, NodeIdentity related = {}) {
                if (!identity.generation) {
                    return false;
                }
                MouseEvent e;
                e.type = type;
                e.client_x = x;
                e.client_y = y;
                e.screen_x = x;
                e.screen_y = y;
                e.time_ms = i.time;
                e.buttons = buttons;
                e.related_target = related;
                return i.vm.dispatch_mouse(identity, e) == DispatchResult::DefaultPrevented;
            };
            if ((input.kind == PointerKind::Move || leave) && i.hovered != target) {
                const auto old = i.chain(i.hovered);
                const auto next = i.chain(target);
                const auto in = [](const auto& nodes, const SceneNode* n) {
                    return std::find(nodes.begin(), nodes.end(), n) != nodes.end();
                };
                emit(i.hovered, MouseType::PointerOut, target);
                emit(i.hovered, MouseType::Out, target);
                for (auto n = old.rbegin(); n != old.rend(); ++n) {
                    if (!in(next, *n)) {
                        emit((*n)->identity, MouseType::PointerLeave, target);
                        emit((*n)->identity, MouseType::Leave, target);
                    }
                }
                emit(target, MouseType::PointerOver, i.hovered);
                emit(target, MouseType::Over, i.hovered);
                for (const auto* n : next) {
                    if (!in(old, n)) {
                        emit(n->identity, MouseType::PointerEnter, i.hovered);
                        emit(n->identity, MouseType::Enter, i.hovered);
                    }
                }
                i.hovered = target;
            }
            switch (input.kind) {
                case PointerKind::Leave:
                    break;
                case PointerKind::Wheel: {
                    WheelEvent e;
                    e.delta_x = input.wheel_x;
                    e.delta_y = input.wheel_y;
                    e.client_x = e.screen_x = x;
                    e.client_y = e.screen_y = y;
                    e.time_ms = i.time;
                    prevented = i.vm.dispatch_wheel(destination, e) == DispatchResult::DefaultPrevented;
                    break;
                }
                case PointerKind::Down:
                    buttons = buttons ? buttons : 1;
                    prevented = emit(destination, MouseType::PointerDown);
                    prevented = emit(destination, MouseType::Down) || prevented;
                    i.pressed = target;
                    break;
                case PointerKind::Up:
                    prevented = emit(destination, MouseType::PointerUp);
                    prevented = emit(destination, MouseType::Up) || prevented;
                    if (target.generation && target == i.pressed) {
                        emit(target, MouseType::Click);
                    }
                    i.pressed = {};
                    break;
                case PointerKind::Move:
                    prevented = emit(destination, MouseType::PointerMove);
                    prevented = emit(destination, MouseType::Move) || prevented;
                    break;
            }
        } catch (const std::runtime_error& e) {
            i.handle(e, ViewDiagnostic::Kind::Script);
        }
        return prevented;
    }

    std::pair<bool, std::string> View::evaluate(std::string_view source) {
        auto& i = *impl_;
        if (!i.loaded || i.faulted) {
            return {false, "WEFT_VIEW_EMPTY: no page"};
        }
        try {
            return {true, i.vm.eval(source, "evaluate")};
        } catch (const std::runtime_error& e) {
            if (faulting(e)) {
                i.handle(e, ViewDiagnostic::Kind::Script);
            }
            return {false, e.what()};
        }
    }

    ViewHit View::hit(float x, float y) {
        auto& i = *impl_;
        if (!std::isfinite(x) || !std::isfinite(y)) {
            throw std::invalid_argument("WEFT_VIEW_POINTER: finite coordinates");
        }
        ViewHit out;
        if (!i.loaded || i.faulted || !i.try_flush()) {
            return out;
        }
        i.take_anchors();
        const auto target = i.geometry.hit_test(i.current, x, y, i.anchors());
        const auto path = i.chain(target);
        // The root and its ancestors are the page, not something on it.
        if (path.empty() || path.back()->identity == i.root_id) {
            return out;
        }
        out.element = target;
        for (auto n = path.rbegin(); n != path.rend(); ++n) {
            const auto cursor = (*n)->style.text(PropertyId::Cursor);
            if (!cursor.empty() && cursor != "inherit" && cursor != "unset") {
                out.cursor = cursor;
                break;
            }
        }
        return out;
    }

    std::vector<ViewDiagnostic> View::take_diagnostics(std::size_t* dropped) {
        auto& i = *impl_;
        if (dropped) {
            *dropped = i.dropped;
        }
        i.dropped = 0;
        std::vector<ViewDiagnostic> out(std::make_move_iterator(i.diagnostics.begin()),
                                        std::make_move_iterator(i.diagnostics.end()));
        i.diagnostics.clear();
        return out;
    }

    std::vector<EngineMessage> View::take_messages() {
        return impl_->vm.take_engine_messages();
    }

    void View::install_function(std::string name, EngineFunction function) {
        impl_->vm.install_engine_function(std::move(name), std::move(function));
    }

    void View::set_resource_reader(ViewResourceReader reader) {
        impl_->reader = std::move(reader);
    }

    bool View::faulted() const {
        return impl_->faulted;
    }

    bool View::loaded() const {
        return impl_->loaded;
    }

    double View::time() const {
        return impl_->time;
    }

    const SceneFrame& View::scene() const {
        return impl_->current;
    }

    ScriptDom& View::script() {
        return impl_->vm;
    }
} // namespace weft
