#pragma once
#include "gallery_scene_manifest.hpp"
#include "gallery_constants.hpp"

struct GalleryOwnerRequest {
    double time{};
    bool audit{};
    std::array<bool, gallery_scene_count> scenes{};
    std::array<bool, caseelephant::probe_constants::gallery::canvas_case_count> canvas{};
};

struct GalleryOwnerInput {
    int scene = -1;
    int canvas = -1;
    caseelephant::ProbeMouseAction action{};
    double x{};
    double y{};
    double time{};
    unsigned test_delay_ms{};
    bool wheel{};
    double delta_y{};
    unsigned buttons{};
    std::chrono::steady_clock::time_point received = std::chrono::steady_clock::now();
};

struct GalleryDetailSample {
    bool enabled{};
    double style_ms{};
    double build_ms{};
    double layout_ms{};
    double geometry_ms{};
    double input_ms{};
    double hit_ms{};
    double dispatch_ms{};
    double input_queue_max_ms{};
    std::size_t inputs{};
    std::size_t layout_reads{};
    std::size_t style_reads{};
    std::array<double, gallery_scene_count> cases{};
};

struct GalleryOwnerFrame {
    std::array<std::shared_ptr<const caseelephant::ProbeSceneFrame>, gallery_scene_count> scenes;
    std::array<std::uint64_t, gallery_scene_count> revisions{};
    std::array<caseelephant::ProbeCanvasSnapshot, caseelephant::probe_constants::gallery::canvas_case_count> canvas;
    std::array<unsigned, caseelephant::probe_constants::gallery::canvas_case_count> raf_calls{};
    double js_ms{};
    double snapshot_ms{};
    double scene_ms{};
    double time{};
    std::string hover_transform;
    std::string toggle_width;
    std::string anime_drag_transform;
    std::size_t snapshots{};
    std::size_t snapshot_reuses{};
    std::size_t evals{};
    std::size_t host_calls{};
    std::size_t mutations{};
    std::size_t snapshot_nodes{};
    std::size_t typed_samples{};
    std::size_t paint_compilations{};
    GalleryDetailSample detail;
};

using GalleryOwnerExecutor =
    caseelephant::OwnerExecutorProbe<GalleryOwnerRequest, GalleryOwnerInput, GalleryOwnerFrame>;

inline GalleryOwnerExecutor::Result gallery_owner_input(GalleryOwnerExecutor& owner, GalleryOwnerInput value) {
    return owner.input_latest(caseelephant::probe_constants::gallery::owner_epoch, std::move(value),
                              [](const auto& a, const auto& b) {
                                  return a.action == caseelephant::ProbeMouseAction::Move && b.action == a.action &&
                                         !a.wheel && !b.wheel && a.scene == b.scene && a.canvas == b.canvas &&
                                         a.buttons == b.buttons && !a.test_delay_ms && !b.test_delay_ms;
                              });
}

class GalleryOwnerState {
    struct ComScope {
        ComScope() {
            check_hresult(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "UI owner COM");
        }

        ~ComScope() {
            CoUninitialize();
        }
    } com_;

    OwnerTrace trace_;
    std::shared_ptr<TraceControl> profile_control_;
    bool profiling_{};
    GalleryDetailSample input_detail_;
    std::vector<std::unique_ptr<GallerySceneSession>> scenes_;
    std::array<std::unique_ptr<GalleryCanvasSession>, caseelephant::probe_constants::gallery::canvas_case_count>
        canvas_;
    std::array<std::shared_ptr<const caseelephant::ProbeSceneFrame>, gallery_scene_count> snapshots_;
    std::array<std::uint64_t, gallery_scene_count> revisions_{};
    std::array<unsigned, caseelephant::probe_constants::gallery::canvas_case_count> raf_{};
    double time_ = caseelephant::probe_constants::gallery::initial_clock_ms;
    bool closed_{};
    unsigned count_;
    std::chrono::steady_clock::time_point current_received_;

public:
    GalleryOwnerState(bool expanded = false, std::filesystem::path trace = {},
                      std::shared_ptr<TraceControl> control = {})
        : trace_(trace, caseelephant::probe_constants::gallery::owner_trace_tid, control),
          profile_control_(std::move(control)),
          count_(expanded ? gallery_scene_count : caseelephant::probe_constants::gallery::core_scene_count) {
        for (unsigned i = 0; i < count_; ++i) {
            const auto d = gallery_scene_descriptors[i];
            scenes_.push_back(std::make_unique<GallerySceneSession>(
                d.group, d.index, d.width, d.height,
                i < caseelephant::probe_constants::gallery::core_scene_count ? "scene-utilities.css"
                                                                             : "cluster-utilities.css"));
        }
        for (unsigned i = 0; i < caseelephant::probe_constants::gallery::canvas_case_count; ++i) {
            canvas_[i] = std::make_unique<GalleryCanvasSession>(i);
        }
        apply_profile();
    }

    void apply_profile() {
        const bool enabled =
            profile_control_ ? profile_control_->enabled.load(std::memory_order_relaxed) : trace_.enabled();
        if (enabled != profiling_) {
            profiling_ = enabled;
            input_detail_ = {};
        }
        for (auto& s : scenes_) {
            if (s->profiling != profiling_) {
                s->set_profiling(profiling_);
            }
        }
    }

    void detail_delta(GalleryDetailSample& out, const GallerySceneSession& s,
                      const GallerySceneSession::ProfileWork& before,
                      const caseelephant::SceneProbe::Work& work_before) {
        const auto w = s.scene.work();
        out.style_ms += w.style_ms - work_before.style_ms;
        out.build_ms += w.build_ms - work_before.build_ms;
        out.layout_ms += w.layout_ms - work_before.layout_ms;
        out.geometry_ms += s.profile.geometry_ms - before.geometry_ms;
        out.hit_ms += s.profile.hit_ms - before.hit_ms;
        out.dispatch_ms += s.profile.dispatch_ms - before.dispatch_ms;
        out.layout_reads += s.profile.layout_reads - before.layout_reads;
        out.style_reads += s.profile.style_reads - before.style_reads;
    }

    void input(const GalleryOwnerInput& value) {
        apply_profile();
        OwnerTrace::Scope trace{trace_, profiling_ ? "input scene=" + std::to_string(value.scene) : std::string{},
                                profiling_ ? "{\"queue_age_ms\":" +
                                                 format_host_time(std::chrono::duration<double, std::milli>(
                                                                      std::chrono::steady_clock::now() - value.received)
                                                                      .count()) +
                                                 ",\"buttons\":" + std::to_string(value.buttons) +
                                                 ",\"action\":" + std::to_string(int(value.action)) +
                                                 ",\"wheel\":" + (value.wheel ? "true" : "false") + "}"
                                           : "{}",
                                profiling_};

        struct InputProfileGuard {
            GalleryOwnerState& owner;
            OwnerTrace::Scope& scope;
            GalleryDetailSample before;
            std::chrono::steady_clock::time_point start;

            ~InputProfileGuard() {
                if (!owner.profiling_) {
                    return;
                }
                const auto ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                auto& d = owner.input_detail_;
                d.input_ms += ms;
                ++d.inputs;
                d.input_queue_max_ms =
                    std::max(d.input_queue_max_ms,
                             std::chrono::duration<double, std::milli>(start - owner.current_received_).count());
                scope.args.pop_back();
                scope.args += ",\"style_ms\":" + format_host_time(d.style_ms - before.style_ms) +
                              ",\"build_ms\":" + format_host_time(d.build_ms - before.build_ms) +
                              ",\"layout_ms\":" + format_host_time(d.layout_ms - before.layout_ms) +
                              ",\"geometry_ms\":" + format_host_time(d.geometry_ms - before.geometry_ms) +
                              ",\"hit_ms\":" + format_host_time(d.hit_ms - before.hit_ms) +
                              ",\"dispatch_ms\":" + format_host_time(d.dispatch_ms - before.dispatch_ms) + "}";
            }
        } guard{*this, trace, input_detail_,
                profiling_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};

        current_received_ = value.received;
        auto operation = [&](GallerySceneSession& session, auto fn) {
            auto before = session.profile;
            auto work = session.scene.work();
            fn();
            if (profiling_) {
                detail_delta(input_detail_, session, before, work);
            }
        };
        if (!std::isfinite(value.time) || !std::isfinite(value.x) || !std::isfinite(value.y) ||
            !std::isfinite(value.delta_y) ||
            value.buttons > caseelephant::probe_constants::gallery::max_mouse_buttons_mask) {
            throw std::invalid_argument("gallery owner finite input");
        }
        time_ = std::max(time_, value.time);
        if (value.test_delay_ms) {
            require(value.test_delay_ms <= caseelephant::probe_constants::gallery::max_test_delay_ms,
                    "owner stress delay bound");
            std::this_thread::sleep_for(std::chrono::milliseconds(value.test_delay_ms));
            return;
        }
        if (value.scene >= int(count_) || value.canvas >= caseelephant::probe_constants::gallery::canvas_case_count ||
            value.scene < -1 || value.canvas < -1) {
            throw std::invalid_argument("gallery owner input index");
        }
        if (value.action == caseelephant::ProbeMouseAction::Cancel) {
            for (auto& s : scenes_) {
                s->time = time_;
                operation(*s, [&] {
                    s->cancel_input();
                });
            }
            return;
        }
        if (value.action == caseelephant::ProbeMouseAction::Move) {
            for (unsigned i = 0; i < count_; ++i) {
                if (int(i) != value.scene && scenes_[i]->hovered.generation) {
                    auto& s = *scenes_[i];
                    s.time = time_;
                    operation(s, [&] {
                        s.pump();
                        s.flush_scene();
                        s.input(caseelephant::ProbeMouseType::Move,
                                caseelephant::probe_constants::gallery::offscreen_pointer,
                                caseelephant::probe_constants::gallery::offscreen_pointer);
                    });
                }
            }
        }
        if (value.scene >= 0) {
            auto& s = *scenes_[unsigned(value.scene)];
            s.time = time_;
            operation(s, [&] {
                s.pump();
                s.flush_scene();
                if (s.svg_geometry) {
                    s.input_value(
                        value.action == caseelephant::ProbeMouseAction::Down ? caseelephant::ProbeMouseType::Down
                        : value.action == caseelephant::ProbeMouseAction::Up ? caseelephant::ProbeMouseType::Up
                                                                             : caseelephant::ProbeMouseType::Move,
                        float(value.x), float(value.y), value.buttons, value.wheel ? value.delta_y : 0);
                } else {
                    if (value.action == caseelephant::ProbeMouseAction::Move) {
                        s.input(caseelephant::ProbeMouseType::Move, float(value.x), float(value.y));
                    }
                    if (value.action == caseelephant::ProbeMouseAction::Down) {
                        s.pressed = s.raster.hit_test(s.current, float(value.x), float(value.y));
                    }
                    if (value.action == caseelephant::ProbeMouseAction::Up) {
                        auto hit = s.raster.hit_test(s.current, float(value.x), float(value.y));
                        if (hit == s.pressed) {
                            s.input(caseelephant::ProbeMouseType::Click, float(value.x), float(value.y));
                        }
                        s.pressed = {};
                    }
                }
            });
        }
        if (value.canvas >= 0) {
            auto& c = *canvas_[unsigned(value.canvas)];
            c.pump(time_);
            if (value.action == caseelephant::ProbeMouseAction::Move) {
                c.input(0, value.x, value.y, time_);
            }
            if (value.action == caseelephant::ProbeMouseAction::Up) {
                c.input(1, value.x, value.y, time_);
            }
        }
    }

    std::shared_ptr<const GalleryOwnerFrame> frame(const GalleryOwnerRequest& request) {
        apply_profile();
        OwnerTrace::Scope trace{trace_, "owner publication", "{}", profiling_};
        if (!std::isfinite(request.time)) {
            throw std::invalid_argument("gallery owner finite time");
        }
        time_ = std::max(time_, request.time);
        auto out = std::make_shared<GalleryOwnerFrame>();
        out->time = time_;
        out->detail = input_detail_;
        out->detail.enabled = profiling_;
        input_detail_ = {};
        for (unsigned i = 0; i < caseelephant::probe_constants::gallery::canvas_case_count; ++i) {
            auto& c = *canvas_[i];
            c.pump(time_);
            auto result = c.vm.animation_frame(time_, request.canvas[i]);
            c.pump(time_);
            raf_[i] += unsigned(result.callbacks);
            require(!result.exceptions && c.vm.stats().listener_exceptions == 0, "worker canvas scheduling");
            out->canvas[i] = c.vm.canvas_snapshot(c.identity());
            out->raf_calls[i] = raf_[i];
        }
        for (unsigned i = 0; i < count_; ++i) {
            auto& s = *scenes_[i];
            const auto trace_start =
                profiling_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const auto before = s.profile;
            const auto work_before = s.scene.work();
            if (request.scenes[i]) {
                s.step(time_, true);
                out->js_ms += s.js_ms;
                out->snapshot_ms += s.snapshot_ms;
                out->scene_ms += s.scene_ms;
            } else {
                s.time = time_;
                s.pump();
                s.vm.animation_frame(time_, false);
                s.pump();
            }
            if (profiling_) {
                const auto elapsed =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - trace_start).count();
                out->detail.cases[i] = elapsed;
                detail_delta(out->detail, s, before, work_before);
            }
            if (profiling_ && trace_.enabled()) {
                const auto elapsed =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - trace_start).count();
                const auto work = s.scene.work();
                if (request.scenes[i] ||
                    elapsed > caseelephant::probe_constants::gallery::hidden_pump_trace_threshold_ms) {
                    trace_.emit(
                        std::string(gallery_scene_descriptors[i].group) + "/" +
                            std::to_string(gallery_scene_descriptors[i].index) +
                            (request.scenes[i] ? " update" : " hidden pump"),
                        trace_start, elapsed,
                        "{\"style_ms\":" + format_host_time(work.style_ms - work_before.style_ms) +
                            ",\"build_ms\":" + format_host_time(work.build_ms - work_before.build_ms) +
                            ",\"layout_ms\":" + format_host_time(work.layout_ms - work_before.layout_ms) +
                            ",\"geometry_ms\":" + format_host_time(s.profile.geometry_ms - before.geometry_ms) +
                            ",\"layout_reads\":" + std::to_string(s.profile.layout_reads - before.layout_reads) +
                            ",\"style_reads\":" + std::to_string(s.profile.style_reads - before.style_reads) + "}");
                }
            }
            if (s.dirty || !snapshots_[i]) {
                snapshots_[i] = std::make_shared<const caseelephant::ProbeSceneFrame>(s.current);
                require(revisions_[i] != UINT64_MAX, "worker scene revision overflow");
                ++revisions_[i];
                s.dirty = false;
            }
            out->scenes[i] = snapshots_[i];
            out->revisions[i] = revisions_[i];
            out->snapshots += s.snapshots;
            out->snapshot_reuses += s.snapshot_reuses;
            auto stats = s.vm.stats();
            out->evals += stats.script_evaluations;
            out->host_calls += stats.host_calls;
            out->mutations += stats.dom_mutations;
            out->snapshot_nodes += stats.snapshot_nodes;
            auto work = s.scene.work();
            out->typed_samples += work.typed_samples;
            out->paint_compilations += work.paint_compilations;
        }
        if (request.audit) {
            out->hover_transform = scenes_[caseelephant::probe_constants::gallery::css_hover_scene]->vm.eval(
                "document.getElementById('root').childNodes[0].style.transform");
            out->toggle_width = scenes_[caseelephant::probe_constants::gallery::css_toggle_scene]->vm.eval(
                "document.getElementById('root').childNodes[0].childNodes[0].childNodes[0].style.width");
            if (count_ > caseelephant::probe_constants::gallery::anime_drag_scene) {
                out->anime_drag_transform = scenes_[caseelephant::probe_constants::gallery::anime_drag_scene]->vm.eval(
                    "document.querySelector('.drag-item').style.transform");
            }
        }
        return out;
    }

    void close() {
        if (closed_) {
            return;
        }
        closed_ = true;
        for (auto& s : scenes_) {
            s->unmount();
            require(s->vm.stats().live_nodes == caseelephant::probe_constants::gallery::unmounted_live_nodes,
                    "worker scene teardown");
        }
        for (auto& c : canvas_) {
            c->vm.eval("__ceGallery.unmount()");
            c->vm.reset_document();
            c->vm.collect_garbage();
            require(c->vm.stats().canvas_contexts == 0, "worker canvas teardown");
        }
        canvas_ = {};
        scenes_.clear();
        closed_ = true;
    }

    ~GalleryOwnerState() {
        try {
            close();
        } catch (...) {
        }
    }
};

std::unique_ptr<GalleryOwnerExecutor> gallery_owner(bool expanded = false, std::filesystem::path trace = {},
                                                    std::shared_ptr<TraceControl> control = {}) {
    return std::make_unique<GalleryOwnerExecutor>([expanded, trace = std::move(trace), control = std::move(control)] {
        auto state = std::make_shared<GalleryOwnerState>(expanded, trace, control);
        GalleryOwnerExecutor::Hooks hooks;
        hooks.input = [state](const auto& value) {
            state->input(value);
        };
        hooks.frame = [state](const auto& request) {
            return state->frame(request);
        };
        hooks.reset = [](std::uint64_t) {
            throw std::logic_error("gallery inspection view reset uses a fresh executor and surface domain");
        };
        hooks.close = [state] {
            state->close();
        };
        return hooks;
    });
}
