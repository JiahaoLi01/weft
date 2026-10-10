#pragma once

#include "probe_common.hpp"
#include "gallery_scene_manifest.hpp"
#include "cheetah_probe.hpp"
#include "gallery_owner_probe.hpp"
#include "gallery_constants.hpp"

namespace probes {
    // Host inspector only. Samples and plots are bounded owned values. Plot/HUD
    // production is throttled to 5 Hz and is never part of an original case's DOM.
    class GalleryLiveProfiler {
        struct Sample {
            double time;
            double value;
        };

        std::map<std::string, std::deque<Sample>> series_;
        std::array<double, gallery_scene_count> last_cases_{};
        std::size_t inputs_{};
        std::size_t layout_reads_{};
        std::size_t style_reads_{};

    public:
        bool enabled{};
        double last_draw = -1;
        std::size_t hud_updates{};
        std::size_t owner_samples{};
        std::size_t render_samples{};

        void clear() {
            series_.clear();
            last_cases_ = {};
            inputs_ = layout_reads_ = style_reads_ = 0;
            last_draw = -1;
        }

        void add(const std::string& name, double time, double value) {
            if (!enabled) {
                return;
            }
            auto& values = series_[name];
            if (values.size() == weft::probe_constants::gallery::sample_window) {
                values.pop_front();
            }
            values.push_back({time, value});
        }

        void owner(double time, const GalleryOwnerExecutor::Publication& publication) {
            if (!enabled || !publication.value->detail.enabled) {
                return;
            }
            ++owner_samples;
            const auto& f = *publication.value;
            const auto& d = f.detail;
            add("Owner", time, publication.active_ms);
            add("JS tasks", time, f.js_ms);
            add("Snapshot", time, f.snapshot_ms);
            add("Style", time, d.style_ms);
            add("Build", time, d.build_ms);
            add("Layout", time, d.layout_ms);
            add("Geometry", time, d.geometry_ms);
            add("Input", time, d.input_ms);
            add("Hit", time, d.hit_ms);
            add("Dispatch", time, d.dispatch_ms);
            add("Input queue", time, d.input_queue_max_ms);
            add("Request wait", time, publication.queue_ms);
            last_cases_ = d.cases;
            inputs_ += d.inputs;
            layout_reads_ += d.layout_reads;
            style_reads_ += d.style_reads;
        }

        std::array<double, 5> stats(const std::string& name) const {
            auto it = series_.find(name);
            if (it == series_.end() || it->second.empty()) {
                return {};
            }
            std::vector<double> v;
            for (const auto& s : it->second) {
                v.push_back(s.value);
            }
            std::sort(v.begin(), v.end());
            return {v.back(), v[(v.size() - 1) / 2], v[std::size_t((v.size() - 1) * .95)], it->second.back().value,
                    v[std::size_t((v.size() - 1) * .99)]};
        }

        FramePacket packet(weft::FontSet& fonts, const WindowFrameRate& fps, const WindowFrameRate& owner_rate,
                           unsigned live, std::size_t pending, double time) {
            ++hud_updates;
            last_draw = time;
            weft::PositionedPage page;
            const std::array<float, 4> clip{2440, 16, 1384, 1328};
            auto rect = [&](float x, float y, float w, float h, std::array<float, 4> color) {
                weft::PositionedPaint item;
                item.paint.kind = weft::PaintKind::Rectangle;
                item.paint.x = x;
                item.paint.y = y;
                item.paint.width = w;
                item.paint.height = h;
                item.paint.srgb = color;
                item.clip = clip;
                page.paints.push_back(std::move(item));
            };
            auto text = [&](std::string value, float x, float y, float size,
                            std::array<float, 4> color = std::array<float, 4>{.83f, .94f, .86f, 1}) {
                weft::PositionedPaint item;
                item.paint.kind = weft::PaintKind::Text;
                item.paint.x = x;
                item.paint.y = y;
                item.paint.font_size = size;
                item.paint.srgb = color;
                item.paint.shaped = fonts.shape(value, size).shaped;
                item.clip = clip;
                page.paints.push_back(std::move(item));
            };
            auto number = [](double v) {
                std::ostringstream s;
                s.imbue(std::locale::classic());
                s << std::fixed << std::setprecision(2) << v;
                return s.str();
            };
            rect(clip[0], clip[1], clip[2], clip[3], {.025f, .037f, .04f, .97f});
            text("Detailed profiler ON | F8 to hide", 2460, 56, 30);
            text("Present " + number(fps.fps) + " FPS | Owner " + number(owner_rate.fps) + " Hz | LIVE " +
                     std::to_string(live) + " | Input pending " + std::to_string(pending),
                 2460, 96, 25);
            text("Latest 600 samples per series | HUD 5 Hz | unthrottled UI", 2460, 132, 24);
            const std::array<std::pair<const char*, std::array<float, 4>>, 5> curves{
                {{"Owner", {1, .75f, .25f, 1}},
                 {"Render CPU", {.4f, .8f, 1, 1}},
                 {"GPU elapsed", {.8f, .5f, 1, 1}},
                 {"Frame age", {1, .45f, .45f, 1}},
                 {"Input queue", {.3f, 1, .6f, 1}}}};
            double scale = weft::probe_constants::gallery::profiler_60hz_frame_ms;
            for (const auto& c : curves) {
                scale = std::max(scale, stats(c.first)[0]);
            }
            scale = std::ceil(scale / weft::probe_constants::gallery::profiler_scale_step_ms) *
                    weft::probe_constants::gallery::profiler_scale_step_ms;
            const float left = 2490;
            const float top = 190;
            const float plot_width = 1280;
            const float plot_height = 270;
            for (double ms : {weft::probe_constants::gallery::profiler_low_line_ms,
                              weft::probe_constants::gallery::profiler_120hz_frame_ms,
                              weft::probe_constants::gallery::profiler_60hz_frame_ms}) {
                float y = top + plot_height - float(ms / scale) * plot_height;
                rect(left, y, plot_width, 1, {.27f, .3f, .3f, 1});
                text(number(ms) + "ms", left + 2, y - 4, 18);
            }
            float lx = left;
            for (const auto& [name, color] : curves) {
                text(std::string(name) + " " + number(stats(name)[3]), lx, 175, 19, color);
                lx += 250;
                auto found = series_.find(name);
                if (found == series_.end()) {
                    continue;
                }

                struct Bucket {
                    bool seen{};
                    double low{};
                    double high{};
                    double last{};
                };

                std::array<Bucket, weft::probe_constants::gallery::profiler_plot_buckets> buckets{};
                for (const auto& sample : found->second) {
                    if (sample.time < time - weft::probe_constants::gallery::profiler_plot_seconds ||
                        sample.time > time) {
                        continue;
                    }
                    auto& b = buckets[std::min<std::size_t>(
                        weft::probe_constants::gallery::profiler_plot_buckets - 1,
                        std::size_t((sample.time - (time - weft::probe_constants::gallery::profiler_plot_seconds)) /
                                    weft::probe_constants::gallery::profiler_plot_seconds *
                                    weft::probe_constants::gallery::profiler_plot_buckets))];
                    if (!b.seen) {
                        b.seen = true;
                        b.low = b.high = sample.value;
                    }
                    b.low = std::min(b.low, sample.value);
                    b.high = std::max(b.high, sample.value);
                    b.last = sample.value;
                }
                float px{};
                float py{};
                bool previous{};
                for (unsigned i = 0; i < buckets.size(); ++i) {
                    const auto& b = buckets[i];
                    if (!b.seen) {
                        continue;
                    }
                    const float x = left + plot_width * i / weft::probe_constants::gallery::profiler_plot_buckets;
                    const float y = top + plot_height - float(b.last / scale) * plot_height;
                    const float hi = top + plot_height - float(b.high / scale) * plot_height;
                    const float lo = top + plot_height - float(b.low / scale) * plot_height;
                    rect(x, hi, 2, std::max(2.f, lo - hi), color);
                    if (previous) {
                        rect(px, py, std::max(2.f, x - px), 2, color);
                        rect(x, std::min(y, py), 2, std::max(2.f, std::abs(py - y)), color);
                    }
                    previous = true;
                    px = x;
                    py = y;
                }
            }
            text("5 seconds | scale 0.." + number(scale) + " ms | GPU elapsed includes queue gaps", 2460, 496, 23);
            text("Phase (ms)                   Latest        P50        P95        P99        Max", 2460, 537, 25);
            const std::array<const char*, 21> rows{
                "Owner",     "JS tasks",    "Style",        "Build",       "Layout",      "Geometry",
                "Snapshot",  "Input",       "Hit",          "Dispatch",    "Input queue", "Request wait",
                "Frame age", "Render CPU",  "Shell layout", "Shell input", "Raster",      "Prepare",
                "Encode",    "Present CPU", "Pipeline wait"};
            for (unsigned i = 0; i < rows.size(); ++i) {
                auto v = stats(rows[i]);
                float y = 574 + i * 27.f;
                text(rows[i], 2460, y, 22);
                text(number(v[3]), 2890, y, 22);
                text(number(v[1]), 3070, y, 22);
                text(number(v[2]), 3250, y, 22);
                text(number(v[4]), 3430, y, 22);
                text(number(v[0]), 3610, y, 22);
            }
            text("Inputs " + std::to_string(inputs_) + " | sync layout reads " + std::to_string(layout_reads_) +
                     " | style reads " + std::to_string(style_reads_),
                 2460, 1180, 23);
            std::vector<std::pair<double, unsigned>> hot;
            for (unsigned i = 0; i < last_cases_.size(); ++i) {
                hot.push_back({last_cases_[i], i});
            }
            std::sort(hot.rbegin(), hot.rend());
            std::string line = "Last owner update: ";
            for (unsigned i = 0; i < weft::probe_constants::gallery::profiler_hot_case_count && i < hot.size(); ++i) {
                const auto d = gallery_scene_descriptors[hot[i].second];
                line += std::string(d.group) + "/" + std::to_string(d.index) + " " + number(hot[i].first) + "ms  ";
            }
            text(line, 2460, 1220, 20);
            text("Nested phase times overlap; trace retains its latest bounded window.", 2460, 1258, 22);
            text("Esc closes and writes owner-trace.json + render-trace.json.", 2460, 1296, 22);
            return cheetah_packet(page, fonts, {});
        }
    };
} // namespace probes
