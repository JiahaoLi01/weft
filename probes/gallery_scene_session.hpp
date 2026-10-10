#pragma once
#define NOMINMAX
#include "gallery_scene_manifest.hpp"
#include "caseelephant/script_platform.hpp"
#include "gallery_constants.hpp"
#include "caseelephant/scene_probe.hpp"
#include "../tests/support/files.hpp"
#include "../tests/support/host_time.hpp"
#include <fstream>
#include <chrono>
#include <sstream>
#include <windows.h>

inline std::string scene_read(const std::string& path) {
    return caseelephant::testing::read_file(path);
}

// One owner VM per original case. The fixture and its React/D3 bundles are
// unchanged. The declared host is the same body formatting context as gallery.
struct GallerySceneSession {
    caseelephant::SceneRasterProbe raster;
    caseelephant::SceneProbe scene;
    caseelephant::ScriptDomProbe vm{{caseelephant::probe_constants::gallery::vm_stack_bytes,
                                     caseelephant::probe_constants::gallery::scene_vm_execution_budget_ms}};
    caseelephant::ProbeSceneFrame current;
    caseelephant::ProbeNodeIdentity hovered{};
    caseelephant::ProbeNodeIdentity pressed{};
    bool input_down{};
    double js_ms{};
    double snapshot_ms{};
    double scene_ms{};
    bool profiling = true;

    struct ProfileWork {
        std::size_t layout_reads{};
        std::size_t style_reads{};
        std::size_t flushes{};
        std::size_t hits{};
        std::size_t dispatches{};
        std::size_t scene_builds{};
        std::size_t geometry_builds{};
        double layout_ms{};
        double style_ms{};
        double flush_ms{};
        double hit_ms{};
        double dispatch_ms{};
        double scene_build_ms{};
        double geometry_ms{};
    } profile;

    struct ProfileScope {
        std::size_t& calls;
        double& duration;
        std::chrono::steady_clock::time_point start;
        bool enabled;

        ProfileScope(std::size_t& c, double& d, bool on)
            : calls(c),
              duration(d),
              start(on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}),
              enabled(on) {
            ++calls;
        }

        ~ProfileScope() {
            if (enabled) {
                duration += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            }
        }
    };

    caseelephant::ProbeNodeIdentity root_id{};
    std::uint64_t dom_revision{};
    std::size_t snapshots{};
    std::size_t snapshot_reuses{};
    float width;
    float height;
    float last_scene_width = -1;
    float last_scene_height = -1;
    double time{};
    bool dirty = true;
    bool svg_geometry{};
    std::optional<caseelephant::ProbeDomSnapshot> last_dom;
    caseelephant::ProbeSceneBitmap cached;
    float cached_scale{};
    double last_scene_time = -1;
    double cache_time = -1;
    float cache_width = -1;
    float cache_height = -1;
    std::uint64_t layout_epoch = 1;
    bool geometry_valid{};

    GallerySceneSession(bool d3, unsigned index, float w = caseelephant::probe_constants::gallery::d3_card_size)
        : GallerySceneSession(d3 ? "D3_TESTS" : "CSS_TESTS", index, w,
                              d3 ? static_cast<float>(caseelephant::probe_constants::gallery::d3_card_size)
                                 : static_cast<float>(caseelephant::probe_constants::gallery::css_card_height)) {
    }

    GallerySceneSession(std::string group, unsigned index, float w, float h,
                        std::string stylesheet = "scene-utilities.css")
        : scene{scene_read(std::string(CE_SOURCE_DIR) + "/examples/crpg-gallery/" + stylesheet),
                scene_read(std::string(CE_SOURCE_DIR) + "/examples/crpg-gallery/" +
                           (stylesheet == "cluster-utilities.css" ? "cluster-keyframes.css" : "scene-keyframes.css")),
                [this](auto text, const auto& style) {
                    return raster.measure(text, style);
                }},
          vm{{std::size_t(group == "CSS_TESTS" || group == "D3_TESTS"
                              ? caseelephant::probe_constants::gallery::vm_stack_bytes
                              : caseelephant::probe_constants::gallery::cluster_vm_stack_bytes),
              caseelephant::probe_constants::gallery::scene_vm_execution_budget_ms}},
          width(w),
          height(h) {
        caseelephant::install_script_platform(vm, caseelephant::ScriptPlatformModule::WebHost);
        caseelephant::install_script_platform(vm, caseelephant::ScriptPlatformModule::AnimationFrame);
        vm.eval(scene_read(CE_SOURCE_DIR "/examples/crpg-gallery/native/platform.js"));
        if (group != "CSS_TESTS" && group != "D3_TESTS") {
            svg_geometry = true;
            vm.install_integer_format_host([](std::int64_t value) {
                auto input = std::to_wstring(value);
                wchar_t decimal[] = L".";
                wchar_t separator[] = L",";
                NUMBERFMTW format{0, 1, 3, decimal, separator, 1};
                wchar_t output[caseelephant::probe_constants::gallery::number_format_buffer_chars]{};
                if (!GetNumberFormatEx(L"en-US", 0, input.c_str(), &format, output,
                                       caseelephant::probe_constants::gallery::number_format_buffer_chars)) {
                    throw std::runtime_error("CE_NUMBER_FORMAT_HOST");
                }
                std::string result;
                for (auto c : output) {
                    if (!c) {
                        break;
                    }
                    if (c > caseelephant::probe_constants::gallery::max_ascii_code_unit) {
                        throw std::runtime_error("CE_NUMBER_FORMAT_ENCODING");
                    }
                    result += char(c);
                }
                return result;
            });
            caseelephant::install_script_platform(vm, caseelephant::ScriptPlatformModule::ScenePlatform);
            vm.eval("globalThis.innerWidth=" + caseelephant::testing::format_host_time(w) +
                    ";globalThis.innerHeight=" + caseelephant::testing::format_host_time(h));
        }
        vm.eval(
            "(()=>{let seed=0xce202610;Math.random=()=>((seed=(Math.imul(seed,1664525)+1013904223)>>>0)/4294967296)})()");
        vm.eval(scene_read(std::string(CE_SOURCE_DIR) + "/examples/crpg-gallery/native/" + group + ".js"));
        vm.eval(
            "document.getElementById('root').setAttribute('style','position:relative;overflow:hidden;background-color:#050403;color:#f5e6c8;font-family:system-ui;font-size:16px;line-height:1.5;" +
            std::string(group == "D3_TESTS" ? "display:flex;align-items:center;justify-content:center;" : "") + "')");
        vm.install_layout_cache_host([this] {
            if (cache_time != time || cache_width != width || cache_height != height) {
                if (layout_epoch == UINT64_MAX) {
                    throw std::overflow_error("scene layout epoch");
                }
                ++layout_epoch;
                cache_time = time;
                cache_width = width;
                cache_height = height;
            }
            return layout_epoch;
        });
        auto root = [](const auto& document) {
            auto find = [&](auto&& self, const auto& n) -> const caseelephant::ProbeDomSnapshot* {
                if (n.attributes.contains("id") && n.attributes.at("id") == "root") {
                    return &n;
                }
                for (const auto& c : n.children) {
                    if (auto r = self(self, c)) {
                        return r;
                    }
                }
                return nullptr;
            };
            auto* value = find(find, document);
            if (!value) {
                throw std::runtime_error("CE_SCENE_ROOT");
            }
            return value;
        };
        vm.install_layout_host([this, root](const auto& document) {
            ProfileScope timer(profile.layout_reads, profile.layout_ms, profiling);
            sync_scene(*root(document));
            ensure_geometry();
            return current.elements;
        });
        vm.install_computed_style_host([this, root](const auto& document, auto identity) {
            ProfileScope timer(profile.style_reads, profile.style_ms, profiling);
            sync_scene(*root(document));
            auto find = [&](auto&& self, const auto& n) -> caseelephant::ProbeSceneStyle {
                if (n.identity == identity) {
                    auto s = n.style;
                    s["width"] = caseelephant::testing::format_host_time(n.box[2]) + "px";
                    s["height"] = caseelephant::testing::format_host_time(n.box[3]) + "px";
                    if (!s.contains("transform") || s["transform"].empty() || s["transform"] == "none") {
                        s["transform"] = "none";
                    } else {
                        auto m = raster.transform_matrix(n);
                        s["transform"] = "matrix(";
                        for (unsigned i = 0; i < 6; ++i) {
                            if (i) {
                                s["transform"] += ",";
                            }
                            s["transform"] += caseelephant::testing::format_host_time(m[i]);
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
        vm.eval("__ceGallery.mount('" + group + "'," + std::to_string(index) + ")");
        step(0);
    }

    void set_profiling(bool enabled) {
        profiling = enabled;
        scene.set_profiling(enabled);
    }

    auto clock() const {
        return profiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    }

    void pump() {
        for (unsigned i = 0; i < caseelephant::probe_constants::gallery::task_pump_turn_limit; ++i) {
            if (vm.pump_tasks(time).ready == 0) {
                return;
            }
        }
        throw std::runtime_error("CE_SCENE_TASK_LIMIT");
    }

    void step(double stamp, bool drawable = true) {
        auto start = clock();
        auto elapsed = [&] {
            return std::chrono::duration<double, std::milli>(clock() - start).count();
        };
        time = stamp;
        pump();
        auto f = vm.animation_frame(time, drawable);
        if (f.exceptions) {
            throw std::runtime_error("CE_SCENE_RAF: " + f.exception_messages_json);
        }
        pump();
        if (vm.eval("__ceGallery.state().errors.length") != "0") {
            throw std::runtime_error(vm.eval("JSON.stringify(__ceGallery.state().errors)"));
        }
        js_ms = elapsed();
        flush_scene();
        if (svg_geometry) {
            vm.eval("__ceScenePlatform.deliverObservers()");
            flush_scene();
        }
        scene_ms = elapsed() - js_ms - snapshot_ms;
    }

    void sync_scene(const caseelephant::ProbeDomSnapshot& root) {
        const bool changed = !last_dom || *last_dom != root;
        const bool resized = width != last_scene_width || height != last_scene_height;
        if (changed) {
            last_dom = root;
            ++snapshots;
        } else {
            ++snapshot_reuses;
        }
        if (!changed && !resized && (!current.animated || last_scene_time == time)) {
            current.time_ms = time;
            last_scene_time = time;
            return;
        }
        {
            ProfileScope timer(profile.scene_builds, profile.scene_build_ms, profiling);
            auto geometry =
                geometry_valid ? std::move(current.elements) : std::vector<caseelephant::ProbeElementGeometry>{};
            try {
                scene.update(*last_dom, current, width, height, time);
            } catch (...) {
                scene.reset();
                current = {};
                last_dom.reset();
                geometry_valid = false;
                throw;
            }
            const auto work = scene.work();
            geometry_valid = geometry_valid && !work.geometry_changed;
            if (geometry_valid) {
                current.elements = std::move(geometry);
            }
            dirty |= work.paint_changed;
        }
        last_scene_width = width;
        last_scene_height = height;
        last_scene_time = time;
        dirty = dirty || resized;
    }

    void ensure_geometry() {
        if (!geometry_valid) {
            ProfileScope timer(profile.geometry_builds, profile.geometry_ms, profiling);
            current.elements = raster.geometry(current, svg_geometry);
            geometry_valid = true;
        }
    }

    void flush_scene() {
        ProfileScope timer(profile.flushes, profile.flush_ms, profiling);
        const auto start = clock();
        auto elapsed = [&] {
            return std::chrono::duration<double, std::milli>(clock() - start).count();
        };
        if (!root_id.generation) {
            root_id = vm.node_identity("#root");
        }
        const auto revision = vm.subtree_revision(root_id);
        if (!last_dom || revision != dom_revision) {
            auto root = vm.snapshot_subtree(root_id);
            dom_revision = revision;
            snapshot_ms = elapsed();
            sync_scene(root);
        } else {
            snapshot_ms = elapsed();
            sync_scene(*last_dom);
        }
        ensure_geometry();
        scene_ms = elapsed() - snapshot_ms;
    }

    const caseelephant::ProbeSceneBitmap&
    bitmap(float scale = caseelephant::probe_constants::gallery::scene_raster_scale) {
        if (dirty || cached.rgba.empty() || cached_scale != scale) {
            cached = raster.raster(current, scale);
            cached_scale = scale;
            dirty = false;
        }
        return cached;
    }

    void input(caseelephant::ProbeMouseType type, float x, float y) {
        if (svg_geometry) {
            input_value(type, x, y);
            return;
        }
        auto target = raster.hit_test(current, x, y);
        caseelephant::ProbeMouseEvent e;
        e.type = type;
        e.client_x = x;
        e.client_y = y;
        e.time_ms = time;
        if (type == caseelephant::ProbeMouseType::Move && hovered != target) {
            if (hovered.generation) {
                e.type = caseelephant::ProbeMouseType::Out;
                e.related_target = target;
                vm.dispatch_mouse(hovered, e);
            }
            if (target.generation) {
                e.type = caseelephant::ProbeMouseType::Over;
                e.related_target = hovered;
                vm.dispatch_mouse(target, e);
            }
            hovered = target;
            e.type = type;
        }
        if (target.generation) {
            vm.dispatch_mouse(target, e);
        }
    }

    void input_value(caseelephant::ProbeMouseType type, float x, float y, unsigned buttons = 0, double wheel = 0) {
        flush_scene();
        caseelephant::ProbeNodeIdentity target;
        {
            ProfileScope timer(profile.hits, profile.hit_ms, profiling);
            target = raster.hit_test(current, x, y);
        }
        const auto destination = target.generation ? target : root_id;
        ProfileScope timer(profile.dispatches, profile.dispatch_ms, profiling);
        auto emit = [&](caseelephant::ProbeNodeIdentity identity, caseelephant::ProbeMouseType event_type,
                        caseelephant::ProbeNodeIdentity related = {}) {
            if (!identity.generation) {
                return;
            }
            caseelephant::ProbeMouseEvent e;
            e.type = event_type;
            e.client_x = x;
            e.client_y = y;
            e.time_ms = time;
            e.buttons = buttons;
            e.related_target = related;
            vm.dispatch_mouse(identity, e);
        };
        auto chain = [&](auto id) {
            std::vector<caseelephant::ProbeNodeIdentity> values;
            auto visit = [&](auto&& self, const auto& n) -> bool {
                if (n.identity == id) {
                    values.push_back(n.identity);
                    return true;
                }
                for (const auto& c : n.children) {
                    if (self(self, c)) {
                        values.push_back(n.identity);
                        return true;
                    }
                }
                return false;
            };
            visit(visit, current.root);
            return values;
        };
        if (type == caseelephant::ProbeMouseType::Move && hovered != target) {
            auto old = chain(hovered);
            auto next = chain(target);
            emit(hovered, caseelephant::ProbeMouseType::PointerOut, target);
            emit(hovered, caseelephant::ProbeMouseType::Out, target);
            for (auto n : old) {
                if (std::find(next.begin(), next.end(), n) == next.end()) {
                    emit(n, caseelephant::ProbeMouseType::PointerLeave, target);
                    emit(n, caseelephant::ProbeMouseType::Leave, target);
                }
            }
            emit(target, caseelephant::ProbeMouseType::PointerOver, hovered);
            emit(target, caseelephant::ProbeMouseType::Over, hovered);
            for (auto i = next.rbegin(); i != next.rend(); ++i) {
                if (std::find(old.begin(), old.end(), *i) == old.end()) {
                    emit(*i, caseelephant::ProbeMouseType::PointerEnter, hovered);
                    emit(*i, caseelephant::ProbeMouseType::Enter, hovered);
                }
            }
            hovered = target;
        }
        if (wheel) {
            caseelephant::ProbeWheelEvent e;
            e.delta_y = wheel;
            e.client_x = x;
            e.client_y = y;
            e.time_ms = time;
            vm.dispatch_wheel(destination, e);
        } else if (type == caseelephant::ProbeMouseType::Down) {
            input_down = true;
            buttons = 1;
            emit(destination, caseelephant::ProbeMouseType::PointerDown);
            emit(destination, type);
            pressed = target;
        } else if (type == caseelephant::ProbeMouseType::Up) {
            input_down = false;
            buttons = 0;
            emit(destination, caseelephant::ProbeMouseType::PointerUp);
            emit(destination, type);
            if (target.generation && target == pressed) {
                emit(target, caseelephant::ProbeMouseType::Click);
            }
            pressed = {};
        } else if (type == caseelephant::ProbeMouseType::Click) {
            buttons = 1;
            emit(destination, caseelephant::ProbeMouseType::PointerDown);
            emit(destination, caseelephant::ProbeMouseType::Down);
            buttons = 0;
            emit(destination, caseelephant::ProbeMouseType::PointerUp);
            emit(destination, caseelephant::ProbeMouseType::Up);
            emit(destination, type);
        } else {
            emit(destination, caseelephant::ProbeMouseType::PointerMove);
            emit(destination, type);
        }
    }

    void cancel_input() {
        if (!hovered.generation && !pressed.generation && !input_down) {
            return;
        }
        if (svg_geometry && root_id.generation) {
            caseelephant::ProbeMouseEvent e;
            e.time_ms = time;
            e.type = caseelephant::ProbeMouseType::PointerCancel;
            vm.dispatch_mouse(root_id, e);
            e.type = caseelephant::ProbeMouseType::Up;
            vm.dispatch_mouse(root_id, e);
            input_value(caseelephant::ProbeMouseType::Move, caseelephant::probe_constants::gallery::offscreen_pointer,
                        caseelephant::probe_constants::gallery::offscreen_pointer);
        }
        hovered = pressed = {};
        input_down = false;
    }

    void unmount() {
        vm.eval("__ceGallery.unmount()");
        pump();
        vm.reset_document();
        vm.collect_garbage();
        scene.reset();
        last_dom.reset();
        root_id = {};
        dom_revision = 0;
        last_scene_width = last_scene_height = -1;
        last_scene_time = -1;
        geometry_valid = false;
        current = {};
        cached = {};
        hovered = pressed = {};
    }
};
