#include "view/view.hpp"

#include "base/diagnostic.hpp"
#include "bridge/bridge_value.hpp"
#include "constants/input.hpp"
#include "constants/view.hpp"
#include "dom/focus_rules.hpp"
#include "dom/node.hpp"
#include "scene/scene_values.hpp"
#include "script/script_platform.hpp"
#include "style/values.hpp"
#include "view/view_keys.hpp"
#include "view/view_text_field.hpp"
#include "constants/scene.hpp"
#include "dom/text_field.hpp"
#include "scene/scene_field.hpp"

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

        // HTML's focus rules (dom/focus_rules.hpp) on an element of the scene.
        bool html(const SceneNode& n) {
            return n.space.empty() || n.space == constants::xhtml_namespace_uri;
        }

        auto attributes_of(const SceneNode& n) {
            return [&n](std::string_view name) -> const std::string* {
                const auto it = n.attributes.find(name);
                return it == n.attributes.end() ? nullptr : &it->second;
            };
        }

        // What keys do to an element as their default action (Blink's HTMLElement::HandleKeyboardActivation and its
        // input types): a button (and an input of a button's type) clicks on Enter's keypress and Space's keyup; a
        // checkbox or radio button on Space's keyup only; a link on Enter's keydown.
        enum class Activation : std::uint8_t {
            None,
            Button,
            Checkable,
            Link
        };

        Activation activation(const SceneNode& n) {
            if (!html(n) || dom::disabled_control(n.tag, true, attributes_of(n))) {
                return Activation::None;
            }
            if (n.tag == "button") {
                return Activation::Button;
            }
            if (n.tag == "input") {
                const auto it = n.attributes.find("type");
                const auto type = dom::lower_ascii_copy(it == n.attributes.end() ? "" : it->second);
                if (type == "button" || type == "submit" || type == "reset" || type == "image" || type == "color" ||
                    type == "file") {
                    return Activation::Button;
                }
                if (type == "checkbox" || type == "radio") {
                    return Activation::Checkable;
                }
            }
            if (n.tag == "a" && n.attributes.contains("href")) {
                return Activation::Link;
            }
            return Activation::None;
        }

        // A scroll container the keyboard can scroll: overflow auto or scroll with content past its client box.
        bool overflows(const SceneNode& n) {
            ElementGeometry e;
            e.client_width = std::max(0.f, n.box[2] - scene_values::len(n.style.text(PropertyId::BorderLeftWidth, "0")) -
                                               scene_values::len(n.style.text(PropertyId::BorderRightWidth, "0")));
            e.client_height =
                std::max(0.f, n.box[3] - scene_values::len(n.style.text(PropertyId::BorderTopWidth, "0")) -
                                  scene_values::len(n.style.text(PropertyId::BorderBottomWidth, "0")));
            e.scroll_width = e.client_width;
            e.scroll_height = e.client_height;
            scene_scroll_geometry(n, e);
            return e.wheel_x || e.wheel_y;
        }

        // A code point as UTF-8.
        std::string utf8_text(char32_t c) {
            std::string out;
            if (c < 0x80) {
                out += char(c);
            } else if (c < 0x800) {
                out += char(0xC0 | (c >> 6));
                out += char(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                out += char(0xE0 | (c >> 12));
                out += char(0x80 | ((c >> 6) & 0x3F));
                out += char(0x80 | (c & 0x3F));
            } else {
                out += char(0xF0 | (c >> 18));
                out += char(0x80 | ((c >> 12) & 0x3F));
                out += char(0x80 | ((c >> 6) & 0x3F));
                out += char(0x80 | (c & 0x3F));
            }
            return out;
        }

        // The code points of UTF-8 text (validated by the caller).
        std::vector<char32_t> code_points(std::string_view text) {
            std::vector<char32_t> out;
            for (std::size_t i = 0; i < text.size();) {
                const auto b = static_cast<unsigned char>(text[i]);
                const int n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
                char32_t c = n == 1 ? b : n == 2 ? b & 0x1F : n == 3 ? b & 0x0F : b & 0x07;
                for (int k = 1; k < n && i + k < text.size(); ++k) {
                    c = (c << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
                }
                out.push_back(c);
                i += n;
            }
            return out;
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
        // The hit map of the View's rect in the scene's revision with the anchors' serial it was built for: the pointer
        // events and hit tests of one frame share it (a scene's revision is unique to its content; 0 vouches for
        // nothing).
        std::optional<SceneGeometry::HitMap> hits;
        std::uint64_t hits_revision{};
        std::uint64_t hits_anchors{};
        std::array<float, 4> hits_within{};
        NodeIdentity hovered{};
        NodeIdentity pressed{};
        // Keyboard: after a keydown the page or a default action used, its text is dropped until the next keydown;
        // the last keydown, whose code, location, modifiers and repeat its keypresses carry.
        bool suppress_text{};
        KeyEvent last_key;
        // The scene's elements in tree order with whether each is being rendered, for the scene revision they were
        // listed for (0 vouches for nothing).
        struct SceneElement {
            NodeIdentity identity;
            const SceneNode* node{};
            bool rendered{};
            // The index past its subtree.
            std::size_t end{};
        };
        std::vector<SceneElement> elements;
        std::uint64_t elements_revision{};
        bool elements_valid{};
        std::deque<ViewDiagnostic> diagnostics;
        std::size_t dropped{};
        // The host's UI shader effects.
        const SceneEffects* effects{};
        // The web platform's localStorage and sessionStorage.
        std::shared_ptr<WebStorage> local_storage = std::make_shared<WebStorage>();
        std::shared_ptr<WebStorage> session_storage = std::make_shared<WebStorage>();
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

        // The topmost node at a point of the current scene with the anchors taken last. A point on the View is asked
        // of the hit map of its rect, built again only when the scene, the anchors or the size changed; one off it
        // (a captured pointer's) of the whole scene.
        NodeIdentity hit_test(float x, float y) {
            const auto* table = anchors();
            if (!(x >= 0 && y >= 0 && x <= width && y <= height)) {
                return geometry.hit_map(current, table).hit(x, y);
            }
            const std::uint64_t serial = table ? table->serial() : 0;
            const std::array<float, 4> within{0, 0, width, height};
            if (!hits || !current.revision || hits_revision != current.revision || hits_anchors != serial ||
                hits_within != within) {
                hits = geometry.hit_map(current, table, &within);
                hits_revision = current.revision;
                hits_anchors = serial;
                hits_within = within;
            }
            return hits->hit(x, y);
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
            vm.install_console_host([this](ConsoleLevel level, std::string_view message) {
                diagnose(level == ConsoleLevel::Error     ? ViewDiagnostic::Kind::ConsoleError
                         : level == ConsoleLevel::Warning ? ViewDiagnostic::Kind::ConsoleWarning
                                                          : ViewDiagnostic::Kind::Console,
                         std::string(message));
            });
            vm.install_storage(local_storage, session_storage);
            vm.install_media_host([this](std::string_view query) {
                return media_matches(query, width, height);
            });
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
            // An element takes the focus where the page renders it: not under display: none, and visible.
            vm.install_focus_host([this](const DomSnapshot& document, NodeIdentity identity) {
                sync_read(*root_of(document));
                for (const auto& e : scene_elements()) {
                    if (e.identity == identity) {
                        return e.rendered;
                    }
                }
                return false;
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
                        // A text field's values as its author gave them, not as it lays out and draws itself.
                        if (n.field) {
                            for (const auto& [property, value] : n.field->authored) {
                                if (value) {
                                    s[std::string(property_name(property))] = *value;
                                } else {
                                    s.erase(std::string(property_name(property)));
                                }
                            }
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
            hits.reset();
            elements.clear();
            elements_valid = false;
            suppress_text = false;
            last_key = {};
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

        // The scene's elements in tree order (SceneElement), listed again when the scene changed.
        const std::vector<SceneElement>& scene_elements() {
            if (elements_valid && current.revision && elements_revision == current.revision) {
                return elements;
            }
            elements.clear();
            const auto visit = [&](auto&& self, const SceneNode& n, bool shown) -> void {
                if (n.tag == "#text" || is_generated(n.identity)) {
                    return;
                }
                const bool displayed = shown && n.style.text(PropertyId::Display, "inline") != "none";
                const auto at = elements.size();
                elements.push_back({n.identity, &n, displayed && !scene_values::invisible(n.style), 0});
                for (const auto& c : n.children) {
                    self(self, c, displayed);
                }
                elements[at].end = elements.size();
            };
            visit(visit, current.root, true);
            elements_revision = current.revision;
            elements_valid = true;
            return elements;
        }

        const SceneNode* scene_node(NodeIdentity id) {
            for (const auto& e : scene_elements()) {
                if (e.identity == id) {
                    return e.node;
                }
            }
            return nullptr;
        }

        // An element of sequential focus navigation (Blink's ScopedFocusNavigation, over the scene): its tab index as
        // Blink adjusts it (the tabindex attribute's, else 0 for an element that takes the focus and -1 for another)
        // and whether Tab stops at it.
        struct Stop {
            NodeIdentity identity;
            std::int32_t tab_index{};
            bool visit{};
        };

        std::vector<Stop> navigation_stops() {
            const auto& list = scene_elements();
            std::vector<Stop> stops(list.size());
            // How many stops lie at or after each index: a scroll container the keyboard can scroll takes the focus
            // when none lies inside it (Chrome's keyboard-focusable scrollers), and its subtree comes after it.
            std::vector<std::size_t> after(list.size() + 1);
            for (std::size_t k = list.size(); k-- > 0;) {
                const auto& n = *list[k].node;
                const auto* index_text = n.attributes.contains("tabindex") ? &n.attributes.at("tabindex") : nullptr;
                const auto index = index_text ? dom::html_integer(*index_text) : std::nullopt;
                bool focusable = list[k].rendered && !dom::disabled_control(n.tag, html(n), attributes_of(n)) &&
                                 (index || dom::focusable_by_default(n.tag, html(n), attributes_of(n)));
                if (!focusable && !index && list[k].rendered && overflows(n) && after[k + 1] == after[list[k].end]) {
                    focusable = true;
                }
                const std::int32_t adjusted = index ? *index : focusable ? 0 : -1;
                stops[k] = {list[k].identity, adjusted, focusable && adjusted >= 0};
                after[k] = after[k + 1] + (stops[k].visit ? 1 : 0);
            }
            return stops;
        }

        // Blink's ScopedFocusNavigation::NextFocusableElement from the stop at current (none: from the start).
        static std::optional<std::size_t> next_stop(const std::vector<Stop>& s, std::optional<std::size_t> current) {
            if (current) {
                const auto t = s[*current].tab_index;
                // An element out of the tabbing cycle goes on in tree order.
                if (t < 0) {
                    for (auto k = *current + 1; k < s.size(); ++k) {
                        if (s[k].visit) {
                            return k;
                        }
                    }
                    return std::nullopt;
                }
                for (auto k = *current + 1; k < s.size(); ++k) {
                    if (s[k].visit && s[k].tab_index == t) {
                        return k;
                    }
                }
                if (t == 0) {
                    return std::nullopt;
                }
            }
            // The lowest tab index above current's (above 0 from the start), first in tree order; then the first 0.
            const std::int32_t above = current ? s[*current].tab_index : 0;
            std::optional<std::size_t> winner;
            for (std::size_t k = 0; k < s.size(); ++k) {
                if (s[k].visit && s[k].tab_index > above && (!winner || s[k].tab_index < s[*winner].tab_index)) {
                    winner = k;
                }
            }
            if (winner) {
                return winner;
            }
            for (std::size_t k = 0; k < s.size(); ++k) {
                if (s[k].visit && s[k].tab_index == 0) {
                    return k;
                }
            }
            return std::nullopt;
        }

        // Blink's ScopedFocusNavigation::PreviousFocusableElement from the stop at current (none: from the end).
        static std::optional<std::size_t> previous_stop(const std::vector<Stop>& s,
                                                        std::optional<std::size_t> current) {
            const std::int32_t t = current ? s[*current].tab_index : 0;
            const std::size_t before = current ? *current : s.size();
            if (t < 0) {
                for (auto k = before; k-- > 0;) {
                    if (s[k].visit) {
                        return k;
                    }
                }
                return std::nullopt;
            }
            for (auto k = before; k-- > 0;) {
                if (s[k].visit && s[k].tab_index == t) {
                    return k;
                }
            }
            // The highest positive tab index below current's (any from 0 or the end), last in tree order.
            const std::int32_t below = current && t ? t : std::numeric_limits<std::int32_t>::max();
            std::optional<std::size_t> winner;
            std::int32_t winning = 0;
            for (auto k = s.size(); k-- > 0;) {
                if (s[k].visit && s[k].tab_index < below && s[k].tab_index > winning) {
                    winner = k;
                    winning = s[k].tab_index;
                }
            }
            return winner;
        }

        // Tab and Shift+Tab (FocusController::AdvanceFocus): the next or previous element of sequential focus
        // navigation from where it starts. Past the last element (or before the first) the focus comes round to the
        // first (the last), as in a Chromium embedded with no browser UI to take it. Whether an element took it.
        bool advance_focus(bool forward) {
            const auto stops = navigation_stops();
            std::optional<std::size_t> from;
            if (const auto start = vm.navigation_start(forward)) {
                for (std::size_t k = 0; k < stops.size(); ++k) {
                    if (stops[k].identity == *start) {
                        from = k;
                        break;
                    }
                }
            }
            auto found = forward ? next_stop(stops, from) : previous_stop(stops, from);
            if (!found) {
                found = forward ? next_stop(stops, std::nullopt) : previous_stop(stops, std::nullopt);
            }
            if (!found) {
                return false;
            }
            vm.focus(stops[*found].identity, FocusType::Keyboard, true);
            return true;
        }

        // Keyboard events go to the focused element, else the body.
        NodeIdentity key_target() {
            if (const auto f = vm.focused()) {
                return *f;
            }
            return vm.node_identity("body");
        }

        // Keyboard scrolling starts at the focused element, else where the mouse went down last, else the scene's
        // root (Blink's ScrollManager::LogicalScroll).
        bool key_scroll(ScrollDirection direction, ScrollGranularity granularity) {
            auto start = vm.focused();
            if (!start) {
                start = vm.pressed();
            }
            return vm.scroll_by_key(start.value_or(root_id), direction, granularity);
        }

        // The click a key asks for (Blink's DispatchSimulatedClick).
        void click(NodeIdentity target) {
            MouseEvent e;
            e.type = MouseType::Click;
            e.simulated = true;
            e.time_ms = time;
            vm.dispatch_mouse(target, e);
        }

        // The default actions of a keydown nobody prevented (Blink's element default handlers, then
        // KeyboardEventManager::DefaultKeyboardEventHandler): its input_* bits, and whether it used the key, which
        // drops the text the key types.
        std::pair<unsigned, bool> keydown_default(NodeIdentity target, const KeyEvent& e) {
            if (const auto field = field_keydown(target, e)) {
                return *field;
            }
            const auto* node = scene_node(target);
            const auto kind = node ? activation(*node) : Activation::None;
            const bool shift = e.modifiers & key_modifier::shift;
            const bool control = e.modifiers & key_modifier::control;
            const bool alt = e.modifiers & key_modifier::alt;
            const bool meta = e.modifiers & key_modifier::meta;
            // A button's Space makes it :active (its keyup clicks it) and leaves the key's text to its keypress.
            if (e.key == " " && (kind == Activation::Button || kind == Activation::Checkable)) {
                vm.set_key_active(target);
                return {input_default, false};
            }
            // A focused link's Enter clicks it.
            if (e.key == "Enter" && kind == Activation::Link && vm.focused() == target) {
                click(target);
                return {input_default, true};
            }
            if (e.key == "Tab") {
                if (control || alt || meta) {
                    return {0, false};
                }
                const bool moved = advance_focus(!shift);
                return {moved ? input_default : 0, moved};
            }
            // Arrow, page, Home and End keys scroll (MapKeyCodeForScroll): not with Shift, Meta or (a system key on
            // Windows) Alt; with Control only Home and End.
            if (shift || meta || alt || (control && e.key != "Home" && e.key != "End")) {
                return {0, false};
            }
            std::optional<std::pair<ScrollDirection, ScrollGranularity>> scroll;
            if (e.key == "ArrowUp") {
                scroll.emplace(ScrollDirection::Up, ScrollGranularity::Line);
            } else if (e.key == "ArrowDown") {
                scroll.emplace(ScrollDirection::Down, ScrollGranularity::Line);
            } else if (e.key == "ArrowLeft") {
                scroll.emplace(ScrollDirection::Left, ScrollGranularity::Line);
            } else if (e.key == "ArrowRight") {
                scroll.emplace(ScrollDirection::Right, ScrollGranularity::Line);
            } else if (e.key == "PageUp") {
                scroll.emplace(ScrollDirection::Up, ScrollGranularity::Page);
            } else if (e.key == "PageDown") {
                scroll.emplace(ScrollDirection::Down, ScrollGranularity::Page);
            } else if (e.key == "Home") {
                scroll.emplace(ScrollDirection::Up, ScrollGranularity::Document);
            } else if (e.key == "End") {
                scroll.emplace(ScrollDirection::Down, ScrollGranularity::Document);
            }
            if (scroll && key_scroll(scroll->first, scroll->second)) {
                return {input_default, true};
            }
            return {0, false};
        }

        // A keypress of a character the last key typed at the focused element (the body without one), with its
        // default actions: a button's Enter clicks it and its Space does nothing more; Space elsewhere scrolls a page
        // (Shift: back). Its input_* bits.
        unsigned press(char32_t c) {
            const auto target = key_target();
            KeyEvent e;
            e.type = KeyType::Press;
            e.key = c == U'\r' ? std::string("Enter") : utf8_text(c);
            e.code = last_key.code;
            e.location = last_key.location;
            e.modifiers = last_key.modifiers;
            e.repeat = last_key.repeat;
            // keyCode, which and charCode: the character's first UTF-16 unit.
            e.key_code = e.char_code = c < 0x10000 ? unsigned(c) : unsigned(0xD800 + ((c - 0x10000) >> 10));
            e.time_ms = time;
            if (vm.dispatch_key(target, e) == DispatchResult::DefaultPrevented) {
                return input_prevented;
            }
            // The default actions see what the listeners did.
            try_flush();
            if (const auto field = field_press(target, c)) {
                return *field;
            }
            const auto* node = scene_node(target);
            const auto kind = node ? activation(*node) : Activation::None;
            if (c == U'\r' && kind == Activation::Button) {
                click(target);
                return input_default;
            }
            if (c == U' ' && (kind == Activation::Button || kind == Activation::Checkable)) {
                return input_default;
            }
            if (c == U' ' && !(e.modifiers & (key_modifier::control | key_modifier::alt | key_modifier::meta))) {
                return key_scroll(e.modifiers & key_modifier::shift ? ScrollDirection::Up : ScrollDirection::Down,
                                  ScrollGranularity::Page)
                           ? input_default
                           : 0;
            }
            return 0;
        }

        // The mouse went down on target and nobody prevented it (Blink's MouseEventManager::HandleMouseFocus): the
        // focus goes to it or its nearest ancestor that takes the focus, and away when none does.
        void mouse_focus(NodeIdentity target) {
            std::vector<NodeIdentity> path;
            for (const auto* n : chain(target)) {
                path.push_back(n->identity);
            }
            for (auto n = path.rbegin(); n != path.rend(); ++n) {
                if (vm.focusable(*n)) {
                    vm.focus(*n, FocusType::Mouse, false);
                    return;
                }
            }
            vm.focus(std::nullopt, FocusType::Mouse, false);
        }

        // ---- Text fields (dom/text_field.hpp, SceneNode::field): keys, the mouse and the input method edit the focused
        // one; its caret blinks, and its text scrolls to show the caret. ----

        // The focused element when it is a text field, with its state.
        std::pair<NodeIdentity, std::shared_ptr<const TextFieldState>> focused_field() {
            if (const auto f = vm.focused()) {
                if (auto state = vm.text_field(*f)) {
                    return {*f, std::move(state)};
                }
            }
            return {};
        }

        // A field's scene node and its map from its own px to the View's (ElementGeometry::matrix).
        struct FieldPlace {
            const SceneNode* node{};
            std::array<double, 6> matrix{};
            std::array<double, 4> rect{};
        };

        std::optional<FieldPlace> field_place(NodeIdentity id) {
            const auto* n = scene_node(id);
            if (!n || !n->field) {
                return std::nullopt;
            }
            ensure_geometry();
            for (const auto& e : current.elements) {
                if (e.identity == id) {
                    return FieldPlace{n, e.matrix, e.rect};
                }
            }
            return std::nullopt;
        }

        // A point of the View in an element's px, by the inverse of its map.
        static std::array<float, 2> to_local(const std::array<double, 6>& m, float x, float y) {
            const double det = m[0] * m[3] - m[1] * m[2];
            if (std::abs(det) < 1e-12) {
                return {0, 0};
            }
            const double dx = x - m[4];
            const double dy = y - m[5];
            return {float((m[3] * dx - m[2] * dy) / det), float((-m[1] * dx + m[0] * dy) / det)};
        }

        // The value offset of the caret stop nearest a point of the View in a field.
        std::optional<std::uint32_t> field_offset(NodeIdentity id, const TextFieldState& state, float x, float y) {
            const auto place = field_place(id);
            if (!place) {
                return std::nullopt;
            }
            if (place->node->field->placeholder) {
                return 0;
            }
            const auto local = to_local(place->matrix, x, y);
            return scene_field_value(state, scene_field_offset_at(*place->node, local[0], local[1]));
        }

        // The value offset of the line above or below the focus at its x in a textarea (Up and Down); its start or end
        // past its first or last line.
        // The column Up and Down keep while they move on (Chrome's line direction point): the caret's whole px when the
        // first of them went, until another key or the mouse moves the caret.
        std::optional<float> field_column;

        std::uint32_t field_vertical(NodeIdentity id, const TextFieldState& state, std::uint32_t focus, bool down) {
            const auto* n = scene_node(id);
            if (!n || !n->field || n->field->placeholder) {
                return down ? std::uint32_t(state.value.size()) : 0;
            }
            const auto caret = scene_field_caret(*n, scene_field_shown(state, focus));
            if (!caret) {
                return down ? std::uint32_t(state.value.size()) : 0;
            }
            if (!field_column) {
                field_column = std::floor(caret->x);
            }
            if ((!down && caret->line == 0) || (down && caret->line + 1 >= n->lines.size())) {
                return down ? std::uint32_t(state.value.size()) : 0;
            }
            const auto& to = n->lines[down ? caret->line + 1 : caret->line - 1];
            return scene_field_value(state,
                                     scene_field_offset_at(*n, *field_column, to.rect[1] + to.rect[3] / 2));
        }

        // The start or end of the line a textarea shows the focus on (Home and End move by visual lines), from its
        // laid-out node; its line between line feeds without one.
        std::uint32_t field_line_end(NodeIdentity id, const TextFieldState& state, std::uint32_t focus, bool end) {
            const auto* n = scene_node(id);
            if (n && n->field && !n->field->placeholder) {
                if (const auto caret = scene_field_caret(*n, scene_field_shown(state, focus))) {
                    const auto& l = n->lines[caret->line];
                    return scene_field_value(state, end ? l.start + std::uint32_t(l.text.size()) : l.start);
                }
            }
            const auto line = line_at(state.value, focus);
            return end ? line.second : line.first;
        }

        // What the focused field's text scrolls to show when its value or selection changes next (Chrome's
        // RevealSelection): the focus of the selection after keys and the input method (the extent of what the user
        // selects), nothing after the mouse (it places the caret where it points, even out of sight), else the caret or
        // the start of a range (a focus that selects all shows the start).
        enum class FieldReveal : std::uint8_t {
            Selection,
            Extent,
            None
        };
        FieldReveal field_reveal{};

        // A key at the focused text field (Chrome's editing commands on Windows): the caret moves by character, word
        // (Control), line and the whole value, and Shift extends the selection from its anchor instead (select fires);
        // Left and Right collapse a range to its start and end; Up and Down go to an input's ends, a textarea's next
        // lines. Backspace and Delete delete the selection, else the character (Control: the word) before or after the
        // caret. Control+A selects all; a search field's Escape empties it. A read-only field neither moves its caret
        // nor deletes (its keys act as elsewhere), but selects all. Its input_* bits and that it used the key; none for
        // a key it leaves alone.
        std::optional<std::pair<unsigned, bool>> field_keydown(NodeIdentity target, const KeyEvent& e) {
            const auto [field, state] = focused_field();
            if (!state || field != target || (e.modifiers & (key_modifier::alt | key_modifier::meta))) {
                return std::nullopt;
            }
            const auto& f = *state;
            const bool shift = e.modifiers & key_modifier::shift;
            const bool control = e.modifiers & key_modifier::control;
            const std::string_view value(f.value);
            const auto size = std::uint32_t(f.value.size());
            const auto anchor = f.backward ? f.selection_end : f.selection_start;
            const auto focus = f.backward ? f.selection_start : f.selection_end;
            const bool range = f.selection_start != f.selection_end;
            const std::pair<unsigned, bool> done{input_default, true};
            const auto& key = e.key;
            if (control && !shift && (key == "a" || key == "A")) {
                field_reveal = FieldReveal::Extent;
                vm.select_text_field(field, 0, size, true);
                return done;
            }
            if (key == "Escape" && !control && !shift && f.editable && !f.multiline && !f.value.empty()) {
                const auto* n = scene_node(field);
                const auto type = n ? n->attributes.find("type") : AttributeMap::const_iterator{};
                if (n && type != n->attributes.end() && text_field_type(&type->second) == "search") {
                    field_reveal = FieldReveal::Extent;
                    vm.set_text_field_value(field, "");
                    return done;
                }
                return std::nullopt;
            }
            const bool left = key == "ArrowLeft";
            const bool right = key == "ArrowRight";
            const bool home = key == "Home";
            const bool end = key == "End";
            const bool up = key == "ArrowUp";
            const bool down = key == "ArrowDown";
            if (!up && !down) {
                field_column.reset();
            }
            if (left || right || home || end || up || down) {
                if (!f.editable) {
                    return std::nullopt;
                }
                std::uint32_t to = focus;
                if (!shift && range && !control && (left || right)) {
                    to = left ? f.selection_start : f.selection_end;
                } else if (left) {
                    const auto from = shift || !range ? focus : f.selection_start;
                    to = control ? previous_word(value, from) : previous_character(value, from);
                } else if (right) {
                    const auto from = shift || !range ? focus : f.selection_end;
                    to = control ? next_word(value, from) : next_character(value, from);
                } else if (home || end) {
                    to = f.multiline && !control ? field_line_end(field, f, focus, end) : home ? 0 : size;
                } else {
                    to = f.multiline ? field_vertical(field, f, focus, down) : down ? size : 0;
                }
                field_reveal = FieldReveal::Extent;
                vm.select_text_field(field, shift ? anchor : to, to, shift);
                return done;
            }
            if (key == "Backspace" || key == "Delete") {
                if (!f.editable) {
                    return std::nullopt;
                }
                const bool back = key == "Backspace";
                field_reveal = FieldReveal::Extent;
                if (range) {
                    vm.edit_text_field(field, f.selection_start, f.selection_end, "",
                                       back ? "deleteContentBackward" : "deleteContentForward", false);
                } else if (back) {
                    const auto from = control ? previous_word(value, focus) : previous_character(value, focus);
                    vm.edit_text_field(field, from, focus, "", control ? "deleteWordBackward" : "deleteContentBackward",
                                       false);
                } else {
                    const auto to = control ? next_word(value, focus) : next_character(value, focus);
                    vm.edit_text_field(field, focus, to, "", control ? "deleteWordForward" : "deleteContentForward",
                                       false);
                }
                return done;
            }
            // A key that types (one character, or a dead key's accent to come) types into the field: the page has it
            // (not the host), and its text, which follows, goes in.
            if (f.editable && !control && (key == "Dead" || types_character(key))) {
                return std::pair<unsigned, bool>{input_default, false};
            }
            return std::nullopt;
        }

        // Whether a key's value is one character that types (not a control character).
        static bool types_character(std::string_view key) {
            const auto points = code_points(key);
            return points.size() == 1 && points[0] >= 0x20 && points[0] != 0x7F;
        }

        // A character the user typed at the focused text field (its keypress nobody prevented): it replaces the
        // selection (insertText; a read-only field's beforeinput comes and nothing more). Enter breaks a textarea's
        // line, and in an input fires beforeinput (insertLineBreak) and commits the change (Chrome's implicit
        // submission). Its input_* bits; none when the focus is not on a text field.
        std::optional<unsigned> field_press(NodeIdentity target, char32_t c) {
            const auto [field, state] = focused_field();
            if (!state || field != target) {
                return std::nullopt;
            }
            const auto& f = *state;
            field_reveal = FieldReveal::Extent;
            if (c == U'\r') {
                if (f.multiline) {
                    vm.edit_text_field(field, f.selection_start, f.selection_end, "\n", "insertLineBreak", false);
                } else {
                    if (f.editable) {
                        vm.edit_text_field(field, f.selection_end, f.selection_end, "", "insertLineBreak", false);
                    }
                    vm.commit_text_field_change(field);
                }
                return input_default;
            }
            vm.edit_text_field(field, f.selection_start, f.selection_end, utf8_text(c), "insertText", true);
            return input_default;
        }

        // The mouse in a text field (Blink's SelectionController): a press places the caret (Shift extends the
        // selection to it; a press in the selection waits for the button to come up, where Chrome would start to drag
        // the text), a double press selects a word and a triple one all of an input (a textarea's line); moving with the
        // button down extends the selection by what the press selected; when the button comes up after the mouse made a
        // range, select fires.
        struct FieldDrag {
            NodeIdentity field;
            // 0: no press in a field; 1 by character, 2 by word, 3 by line.
            unsigned granularity{};
            std::uint32_t anchor_start{};
            std::uint32_t anchor_end{};
            std::optional<std::uint32_t> deferred;
            bool ranged{};
        } field_drag;

        void field_mouse_down(NodeIdentity target, float x, float y, unsigned modifiers, unsigned clicks) {
            field_drag = {};
            field_column.reset();
            const auto [field, state] = focused_field();
            if (!state || field != target) {
                return;
            }
            const auto offset = field_offset(field, *state, x, y);
            if (!offset) {
                return;
            }
            const auto& f = *state;
            const std::string_view value(f.value);
            field_reveal = FieldReveal::None;
            if (clicks >= 3 || clicks == 2) {
                const auto r = clicks == 2  ? word_at(value, *offset)
                               : f.multiline ? line_at(value, *offset)
                                             : std::pair<std::uint32_t, std::uint32_t>{0, std::uint32_t(value.size())};
                field_drag = {field, clicks == 2 ? 2u : 3u, r.first, r.second, std::nullopt, r.first != r.second};
                vm.select_text_field(field, r.first, r.second, false);
                return;
            }
            if (modifiers & key_modifier::shift) {
                const auto anchor = f.backward ? f.selection_end : f.selection_start;
                field_drag = {field, 1, anchor, anchor, std::nullopt, anchor != *offset};
                vm.select_text_field(field, anchor, *offset, false);
                return;
            }
            if (f.selection_start != f.selection_end && *offset >= f.selection_start && *offset <= f.selection_end) {
                field_drag = {field, 1, *offset, *offset, *offset, false};
                return;
            }
            field_drag = {field, 1, *offset, *offset, std::nullopt, false};
            vm.select_text_field(field, *offset, *offset, false);
        }

        void field_mouse_move(float x, float y, unsigned buttons) {
            if (!field_drag.granularity) {
                return;
            }
            const auto state = vm.text_field(field_drag.field);
            if (!(buttons & constants::input::primary_button_mask) || !state || vm.focused() != field_drag.field) {
                field_drag = {};
                return;
            }
            const auto offset = field_offset(field_drag.field, *state, x, y);
            if (!offset) {
                return;
            }
            const std::string_view value(state->value);
            if (field_drag.deferred) {
                // A drag from inside the selection selects from where it started (weft drags no text).
                field_drag.anchor_start = field_drag.anchor_end = *field_drag.deferred;
                field_drag.deferred.reset();
            }
            std::uint32_t anchor = field_drag.anchor_start;
            std::uint32_t focus = *offset;
            if (field_drag.granularity > 1) {
                const auto r = field_drag.granularity == 2 ? word_at(value, *offset)
                               : state->multiline         ? line_at(value, *offset)
                                                          : std::pair<std::uint32_t, std::uint32_t>{
                                                       0, std::uint32_t(value.size())};
                if (*offset < field_drag.anchor_start) {
                    anchor = field_drag.anchor_end;
                    focus = r.first;
                } else {
                    focus = std::max(r.second, field_drag.anchor_end);
                }
            }
            field_reveal = FieldReveal::None;
            vm.select_text_field(field_drag.field, anchor, focus, false);
            field_drag.ranged = anchor != focus;
        }

        // After click (and dblclick): a press in the selection places the caret now; a range the mouse made fires
        // select.
        void field_mouse_up() {
            const auto drag = std::exchange(field_drag, {});
            if (!drag.granularity || !vm.text_field(drag.field)) {
                return;
            }
            if (drag.deferred) {
                field_reveal = FieldReveal::None;
                vm.select_text_field(drag.field, *drag.deferred, *drag.deferred, false);
            } else if (drag.ranged) {
                vm.dispatch_select(drag.field);
            }
        }

        // The caret of the focused field: its blink restarts (shown) whenever the field's value, selection or
        // composition changes, and its text scrolls to show it (the focus of a selection the user made, else the
        // start of a range). An input the focus left scrolls back to its start, as Chrome's does.
        NodeIdentity caret_field;
        std::shared_ptr<const TextFieldState> caret_seen;
        double caret_since{};

        // The scroll offsets that show offset (a value offset) in a field's content box, moving its text as little as
        // possible, from its laid-out node.
        std::array<float, 2> revealed(NodeIdentity field, const TextFieldState& state, std::uint32_t offset) {
            std::array<float, 2> scroll{state.scroll_x, state.scroll_y};
            const auto* n = scene_node(field);
            if (!n || !n->field || n->lines.empty()) {
                return scroll;
            }
            const auto& s = n->style;
            const auto edge = [&](PropertyId k) {
                return scene_values::len(s.text(k, "0"));
            };
            // An input shows its text in its content box; a textarea scrolls its padding box, as Chrome's do.
            const bool padded = state.multiline;
            const float left = edge(PropertyId::BorderLeftWidth) + edge(PropertyId::PaddingLeft);
            const float right = edge(PropertyId::BorderRightWidth) + edge(PropertyId::PaddingRight);
            const float top = edge(PropertyId::BorderTopWidth) + (padded ? 0.f : edge(PropertyId::PaddingTop));
            const float bottom =
                edge(PropertyId::BorderBottomWidth) + (padded ? 0.f : edge(PropertyId::PaddingBottom));
            const float content_w = std::max(0.f, n->box[2] - left - right);
            const float content_h = std::max(0.f, n->box[3] - top - bottom);
            const float pad_y = padded ? edge(PropertyId::PaddingTop) + edge(PropertyId::PaddingBottom) : 0.f;
            float text_w{};
            for (const auto& l : n->lines) {
                text_w = std::max(text_w, l.stops.empty() ? 0.f : l.stops.back().second);
            }
            const float caret_w = constants::scene::field_caret_px;
            // The node was laid out at its own state's scroll offsets, which the DOM's may have passed since.
            const auto& laid = *n->field->state;
            const float max_x = std::max(0.f, text_w + caret_w - content_w);
            const float used_x = std::clamp(laid.scroll_x, 0.f, max_x);
            const float text_h = n->lines.back().rect[1] + n->lines.back().rect[3] - n->lines.front().rect[1];
            const float max_y = state.multiline ? std::max(0.f, text_h + pad_y - content_h) : 0.f;
            const float used_y = std::clamp(laid.scroll_y, 0.f, max_y);
            const auto caret =
                scene_field_caret(*n, n->field->placeholder ? 0 : scene_field_shown(state, offset));
            if (!caret) {
                return scroll;
            }
            const auto& l = n->lines[caret->line];
            const float x = caret->x + used_x - left;
            float sx = used_x;
            if (x < sx) {
                sx = x;
            } else if (x + caret_w > sx + content_w) {
                sx = x + caret_w - content_w;
            }
            float sy = used_y;
            if (state.multiline) {
                const float y = l.rect[1] + used_y - top;
                if (y < sy) {
                    sy = y;
                } else if (y + l.rect[3] > sy + content_h) {
                    sy = y + l.rect[3] - content_h;
                }
            }
            return {std::clamp(sx, 0.f, max_x), std::clamp(sy, 0.f, max_y)};
        }

        void field_view() {
            const auto [field, state] = focused_field();
            if (caret_field.generation && caret_field != field) {
                if (const auto left = vm.text_field(caret_field);
                    left && !left->multiline && (left->scroll_x != 0 || left->scroll_y != 0)) {
                    vm.set_text_field_view(caret_field, left->caret_on, 0, 0);
                }
                caret_field = {};
                caret_seen.reset();
            }
            const auto reveal = std::exchange(field_reveal, FieldReveal::Selection);
            if (!state) {
                return;
            }
            const auto& f = *state;
            const bool changed = field != caret_field || !caret_seen || caret_seen->value != f.value ||
                                 caret_seen->selection_start != f.selection_start ||
                                 caret_seen->selection_end != f.selection_end ||
                                 caret_seen->backward != f.backward || caret_seen->composing != f.composing ||
                                 caret_seen->composition_start != f.composition_start ||
                                 caret_seen->composition_end != f.composition_end;
            if (changed) {
                caret_since = time;
            }
            caret_field = field;
            std::array<float, 2> scroll{f.scroll_x, f.scroll_y};
            if (changed && reveal != FieldReveal::None) {
                const auto focus = f.backward ? f.selection_start : f.selection_end;
                scroll = revealed(field, f,
                                  f.selection_start == f.selection_end || reveal == FieldReveal::Extent
                                      ? focus
                                      : f.selection_start);
            }
            const bool on = std::fmod(time - caret_since, 2 * constants::input::caret_blink_ms) <
                            constants::input::caret_blink_ms;
            if (on != f.caret_on || scroll[0] != f.scroll_x || scroll[1] != f.scroll_y) {
                vm.set_text_field_view(field, on, scroll[0], scroll[1]);
            }
            caret_seen = vm.text_field(field);
        }

        // What input at a text field changed reaches the scene, and the field's caret and scrolling follow.
        void refresh_field() {
            if (!caret_field.generation && !focused_field().second) {
                field_reveal = FieldReveal::Selection;
                return;
            }
            try {
                if (try_flush()) {
                    field_view();
                }
            } catch (const std::runtime_error& e) {
                handle(e, ViewDiagnostic::Kind::Script);
            }
        }

        // Click counting (Chrome's, with Windows' double-click time and area): the count of the last press, and when
        // and where it was.
        unsigned click_count{};
        double click_time{};
        float click_x{};
        float click_y{};
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
        i.scene->set_effects(i.effects);
        try {
            i.install_hosts();
            // A page that takes CSS as browsers do takes their selectors in DOM queries too.
            i.vm.set_modern_selectors(page.forgiving_css);
            install_script_platform(i.vm, ScriptPlatformModule::WebHost);
            install_script_platform(i.vm, ScriptPlatformModule::AnimationFrame);
            install_script_platform(i.vm, ScriptPlatformModule::ScenePlatform);
            install_script_platform(i.vm, ScriptPlatformModule::Engine);
            if (page.web_platform) {
                install_script_platform(i.vm, ScriptPlatformModule::WebPlatform);
            }
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
            // The focused text field's caret blinks and its text follows the caret (field_view).
            try {
                i.field_view();
            } catch (const std::runtime_error& e) {
                i.handle(e, ViewDiagnostic::Kind::Script);
            }
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
        const auto target = leave ? NodeIdentity{} : i.hit_test(x, y);
        const auto destination = target.generation ? target : i.root_id;
        bool prevented{};
        unsigned buttons = input.buttons;
        // A press counts on from the last one close to it in time and place (UIEvent.detail); the release and the click
        // carry its count.
        if (input.kind == PointerKind::Down) {
            const bool again = i.click_count && i.time - i.click_time <= constants::input::double_click_ms &&
                               std::abs(x - i.click_x) <= constants::input::double_click_px &&
                               std::abs(y - i.click_y) <= constants::input::double_click_px;
            i.click_count = again ? i.click_count + 1 : 1;
            i.click_time = i.time;
            i.click_x = x;
            i.click_y = y;
        }
        const unsigned clicks = std::max(1u, i.click_count);
        try {
            const auto emit = [&](NodeIdentity identity, MouseType type, NodeIdentity related = {}) {
                if (!identity.generation) {
                    return false;
                }
                // A disabled form control gets no mousedown, mouseup, click or dblclick (Chrome drops them).
                if (type == MouseType::Down || type == MouseType::Up || type == MouseType::Click ||
                    type == MouseType::DblClick) {
                    if (const auto* n = i.scene_node(identity);
                        n && html(*n) && dom::disabled_control(n->tag, true, attributes_of(*n))) {
                        return false;
                    }
                }
                MouseEvent e;
                e.type = type;
                e.clicks = clicks;
                e.ctrl = input.modifiers & key_modifier::control;
                e.shift = input.modifiers & key_modifier::shift;
                e.alt = input.modifiers & key_modifier::alt;
                e.meta = input.modifiers & key_modifier::meta;
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
                case PointerKind::Down: {
                    buttons = buttons ? buttons : 1;
                    prevented = emit(destination, MouseType::PointerDown);
                    const bool down_prevented = emit(destination, MouseType::Down);
                    prevented = down_prevented || prevented;
                    i.pressed = target;
                    // mousedown's default action: the focus moves to what the mouse went down on, and the keyboard
                    // scrolls and navigates from there.
                    i.vm.set_pressed(destination);
                    if (!down_prevented) {
                        i.mouse_focus(target);
                        // In a text field the press places the caret or selects (field_mouse_down).
                        if (i.try_flush()) {
                            i.field_mouse_down(target, x, y, input.modifiers, clicks);
                        }
                    }
                    break;
                }
                case PointerKind::Up:
                    prevented = emit(destination, MouseType::PointerUp);
                    prevented = emit(destination, MouseType::Up) || prevented;
                    if (target.generation && target == i.pressed) {
                        emit(target, MouseType::Click);
                        // The second click of a double click is followed by dblclick.
                        if (clicks == 2) {
                            emit(target, MouseType::DblClick);
                        }
                    }
                    i.pressed = {};
                    i.field_mouse_up();
                    break;
                case PointerKind::Move:
                    prevented = emit(destination, MouseType::PointerMove);
                    prevented = emit(destination, MouseType::Move) || prevented;
                    if (i.field_drag.granularity && i.try_flush()) {
                        i.field_mouse_move(x, y, buttons);
                    }
                    break;
            }
        } catch (const std::runtime_error& e) {
            i.handle(e, ViewDiagnostic::Kind::Script);
        }
        i.refresh_field();
        return prevented;
    }

    unsigned View::key(const KeyInput& input) {
        auto& i = *impl_;
        if (i.faulted) {
            throw ViewError("WEFT_VIEW_FAULTED: load the page again");
        }
        if (input.key.empty() || input.key.size() > constants::input::max_key_bytes ||
            input.code.size() > constants::input::max_key_bytes || !bridge::utf8(input.key) ||
            !bridge::utf8(input.code) || (input.modifiers & ~key_modifier::all)) {
            throw std::invalid_argument("WEFT_VIEW_KEY: a key of 1 to 64 bytes, a code of at most 64, of UTF-8, and modifiers");
        }
        if (!i.loaded || !i.try_flush()) {
            return 0;
        }
        unsigned result{};
        try {
            KeyEvent e;
            e.type = input.down ? KeyType::Down : KeyType::Up;
            // A key an input method took is "Process" with keyCode 229 going down (Chrome's VK_PROCESSKEY).
            const bool ime = input.down && input.ime;
            e.key = ime ? std::string("Process") : input.key;
            e.code = input.code;
            e.location = key_location(input.code);
            e.modifiers = input.modifiers;
            e.repeat = input.down && input.repeat;
            e.key_code = ime              ? constants::input::ime_process_key_code
                         : input.key_code ? input.key_code
                                          : layout_key_code(input.code, input.modifiers);
            e.time_ms = i.time;
            // isComposing: a composition goes on in the focused text field as the key comes (Blink asks the input
            // method controller).
            const auto state = i.focused_field().second;
            e.composing = state && state->composing;
            const auto target = i.key_target();
            if (input.down) {
                i.suppress_text = false;
                i.last_key = e;
                if (i.vm.dispatch_key(target, e) == DispatchResult::DefaultPrevented) {
                    i.suppress_text = true;
                    return input_prevented;
                }
                if (ime) {
                    return 0;
                }
                // The default actions see what the listeners did.
                i.try_flush();
                const auto [bits, used] = i.keydown_default(target, e);
                result = bits;
                i.suppress_text = used;
                // The character Enter types ('\r') reaches the page as Enter's keypress (Chrome on Windows).
                if (!used && e.key == "Enter" &&
                    !(e.modifiers & (key_modifier::control | key_modifier::alt | key_modifier::meta))) {
                    result |= i.press(U'\r');
                }
            } else {
                const bool prevented = i.vm.dispatch_key(target, e) == DispatchResult::DefaultPrevented;
                result = prevented ? input_prevented : 0;
                // Space's keyup clicks the element its keydown made :active, if it still has the focus.
                if (e.key == " ") {
                    const auto active = i.vm.key_active();
                    i.vm.set_key_active(std::nullopt);
                    if (!prevented && active && *active == target) {
                        i.click(target);
                        result |= input_default;
                    }
                }
            }
        } catch (const std::runtime_error& e) {
            i.handle(e, ViewDiagnostic::Kind::Script);
        }
        i.refresh_field();
        return result;
    }

    unsigned View::text(std::string_view text) {
        auto& i = *impl_;
        if (i.faulted) {
            throw ViewError("WEFT_VIEW_FAULTED: load the page again");
        }
        if (text.size() > constants::input::max_text_bytes || !bridge::utf8(text)) {
            throw std::invalid_argument("WEFT_VIEW_TEXT: UTF-8 text of at most 4096 bytes");
        }
        if (!i.loaded || !i.try_flush()) {
            return 0;
        }
        // The key that typed it was used: its characters go with it.
        if (i.suppress_text) {
            return text.empty() ? 0 : input_default;
        }
        unsigned result{};
        try {
            for (const char32_t c : code_points(text)) {
                if (c < 0x20 || c == 0x7F) {
                    continue;
                }
                result |= i.press(c);
            }
        } catch (const std::runtime_error& e) {
            i.handle(e, ViewDiagnostic::Kind::Script);
        }
        i.refresh_field();
        return result;
    }

    bool View::compose(std::string_view text, std::uint32_t selection_start, std::uint32_t selection_end) {
        auto& i = *impl_;
        if (i.faulted) {
            throw ViewError("WEFT_VIEW_FAULTED: load the page again");
        }
        if (text.size() > constants::input::max_text_bytes || !bridge::utf8(text)) {
            throw std::invalid_argument("WEFT_VIEW_TEXT: UTF-8 text of at most 4096 bytes");
        }
        if (!i.loaded || !i.try_flush()) {
            return false;
        }
        const auto [field, state] = i.focused_field();
        if (!state || !state->editable) {
            return false;
        }
        try {
            i.field_reveal = Impl::FieldReveal::Extent;
            i.vm.compose_text_field(field, text, selection_start, selection_end);
        } catch (const std::runtime_error& e) {
            i.handle(e, ViewDiagnostic::Kind::Script);
        }
        i.refresh_field();
        return true;
    }

    bool View::commit(std::string_view text) {
        auto& i = *impl_;
        if (i.faulted) {
            throw ViewError("WEFT_VIEW_FAULTED: load the page again");
        }
        if (text.size() > constants::input::max_text_bytes || !bridge::utf8(text)) {
            throw std::invalid_argument("WEFT_VIEW_TEXT: UTF-8 text of at most 4096 bytes");
        }
        if (!i.loaded || !i.try_flush()) {
            return false;
        }
        const auto [field, state] = i.focused_field();
        if (!state || !state->editable) {
            return false;
        }
        try {
            i.field_reveal = Impl::FieldReveal::Extent;
            i.vm.commit_text_field(field, text);
        } catch (const std::runtime_error& e) {
            i.handle(e, ViewDiagnostic::Kind::Script);
        }
        i.refresh_field();
        return true;
    }

    ViewTextInput View::text_input() {
        auto& i = *impl_;
        ViewTextInput out;
        if (!i.loaded || i.faulted || !i.vm.page_focused() || !i.try_flush()) {
            return out;
        }
        const auto [field, state] = i.focused_field();
        if (!state || !state->editable) {
            return out;
        }
        out.active = true;
        out.composing = state->composing;
        out.password = state->password;
        const auto place = i.field_place(field);
        if (!place) {
            return out;
        }
        const auto& n = *place->node;
        out.field = {float(place->rect[0]), float(place->rect[1]), float(place->rect[2]), float(place->rect[3])};
        const auto focus = state->backward ? state->selection_start : state->selection_end;
        const auto caret = scene_field_caret(n, n.field->placeholder ? 0 : scene_field_shown(*state, focus));
        if (!caret) {
            return out;
        }
        const auto& l = n.lines[caret->line];
        const auto& m = place->matrix;
        float left = std::numeric_limits<float>::infinity();
        float top = left;
        float right = -left;
        float bottom = -left;
        for (const auto& [x, y] : {std::pair{caret->x, l.rect[1]}, std::pair{caret->x + 1, l.rect[1]},
                                   std::pair{caret->x, l.rect[1] + l.rect[3]},
                                   std::pair{caret->x + 1, l.rect[1] + l.rect[3]}}) {
            const float vx = float(m[0] * x + m[2] * y + m[4]);
            const float vy = float(m[1] * x + m[3] * y + m[5]);
            left = std::min(left, vx);
            right = std::max(right, vx);
            top = std::min(top, vy);
            bottom = std::max(bottom, vy);
        }
        out.caret = {left, top, right - left, bottom - top};
        return out;
    }

    void View::set_focus(bool focused) {
        auto& i = *impl_;
        if (i.faulted) {
            throw ViewError("WEFT_VIEW_FAULTED: load the page again");
        }
        try {
            i.vm.set_page_focus(focused);
        } catch (const std::runtime_error& e) {
            i.handle(e, ViewDiagnostic::Kind::Script);
        }
    }

    bool View::focused() const {
        return impl_->vm.page_focused();
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
        const auto target = i.hit_test(x, y);
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

    void View::set_local_storage(std::shared_ptr<WebStorage> storage) {
        if (!storage) {
            throw std::invalid_argument("WEFT_STORAGE: a storage area");
        }
        impl_->local_storage = std::move(storage);
        if (impl_->loaded) {
            impl_->vm.install_storage(impl_->local_storage, impl_->session_storage);
        }
    }

    const std::shared_ptr<WebStorage>& View::local_storage() const {
        return impl_->local_storage;
    }

    void View::set_effects(const SceneEffects* effects) {
        impl_->effects = effects;
        if (impl_->scene) {
            impl_->scene->set_effects(effects);
        }
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
