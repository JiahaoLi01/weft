#include "platform/win32/system_fonts.hpp"
#include "render/backends/d3d11/scene_renderer.hpp"
#include "scene/scene_anchor.hpp"
#include "view/view.hpp"

#include "../tests/support/png.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The CRPG UI's floating UI (health bars, damage numbers, status flags; docs/engine-communication.md, 13.4) over one
// simulated battle, driven two ways (examples/crpg-gallery/native/FLOATING_TESTS.js, tools/build_floating_page.mjs):
//   json      the captured FloatingUIOverlay: every frame the engine pushes the whole batch of instances
//             (__floating_ui_batch through the CRPG bridge), as UWebUIFloatingManager does every 16 ms, and React renders
//             every instance again;
//   anchored  the same instance components: every frame the engine writes each instance's anchor; an instance that
//             comes or goes is an event (floating.spawn, floating.despawn), its changed fields the view model
//             "floating".
// The battle: units with health bars move about at their own distances (which scale them; some leave the screen or the
// visible range); two damage numbers (on two units) every 12 frames for 1.75 s, a status flag every 45 frames for
// 1.7 s; each damage takes health, a unit at none lies dead until the next hit raises it. The hits come in volleys
// because the page's AnimatePresence (framer-motion 12.38) removes the instances that went only once every exit has
// ended: exits (0.15 s) that overlap without a break, as hits 6 frames apart would, keep every damage number of the
// battle in the document (the library's rule, whatever runs it). After the battle a quiet spell: no more hits, every
// unit healed; a unit whose bar pulsed critical gets a new bar (the pulse's exit takes its infinite repeat, in Chrome
// as here, so the old bar would animate for ever). The page is a UI of scale 2 over a 2560 x 1440 game (a View of 1280
// x 720 CSS px at scale 2, as the game's <html> zoom of 2 lays it out), drawn by the native D3D11 scene pass (hardware,
// or WARP). Timed per frame: the engine's work (the JSON, or the anchors and events), the View's advance (events,
// React, styles, layout, the scene), the pass's CPU and its GPU time. At set frames both ways are drawn and compared:
// alike but for the svg icons the anchored way moves on the GPU (their coverage drawn at the element's origin and own
// scale, and resampled where and as large as the anchor puts it, where the JSON way rasterizes it in place): pixels
// more than 2 levels apart make regions no larger than an icon and its shadow (64 px), whose ink lies in the same place
// (its centroid within half a pixel) in nearly the same amount in every channel (within 10% of its coverage). A glyph
// or an icon a pixel off, or another one, differs by more.
//
//   weft_floating_benchmark --quick <folder>   WARP, 12 units: both ways draw alike, the anchored way stops publishing
//                                              frames in the quiet spell, the JSON way does not, no page diagnostics
//   weft_floating_benchmark --full <folder>    this machine's GPU: 12, 50 and 200 units, 60 + 600 frames each
namespace {
    using Clock = std::chrono::steady_clock;
    namespace d3d = weft::render::d3d11;
    using Microsoft::WRL::ComPtr;

    constexpr unsigned game_width = 2560;
    constexpr unsigned game_height = 1440;
    constexpr float ui_scale = 2;
    constexpr double frame_s = 1.0 / 60;
    constexpr unsigned damage_every = 12;
    constexpr unsigned damage_volley = 2;
    // Instances that went and may stay in the document in the battle: a volley's, a flag's whose exit runs into the
    // next volley's, and that volley's.
    constexpr std::size_t most_left = 2 * damage_volley + 1;
    constexpr unsigned flag_every = 45;
    constexpr double damage_ttl = 1.75;
    constexpr double flag_ttl = 1.7;
    constexpr double max_distance = 5000;
    constexpr double max_health = 100;
    // HealthBar pulses below this share of health.
    constexpr double critical_health = 0.3;
    // How the two ways' drawings may differ (see above).
    constexpr int alike_levels = 2;
    constexpr int alike_merge = 3;
    // Around a region, the ink of the features it cuts through.
    constexpr int alike_margin = 8;
    constexpr int alike_region = 64;
    constexpr double alike_shift = 0.5;
    constexpr double alike_ink = 0.1;

    std::string read(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("cannot read " + path.string());
        }
        return {std::istreambuf_iterator<char>(in), {}};
    }

    double percentile(std::vector<double> v, double p) {
        if (v.empty()) {
            return 0;
        }
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, std::size_t(std::ceil(p * double(v.size()))) - 1)];
    }

    std::string stats(const std::vector<double>& v) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(3) << "{\"p50\":" << percentile(v, .5)
            << ",\"p95\":" << percentile(v, .95) << ",\"p99\":" << percentile(v, .99) << ",\"max\":" << percentile(v, 1)
            << '}';
        return out.str();
    }

    double since(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    // A number as JSON writes it: the shortest that reads back the same.
    std::string number(double v) {
        char text[32];
        const auto end = std::to_chars(text, text + sizeof text, v).ptr;
        return {text, end};
    }

    // An instance of the floating UI as the engine keeps it.
    struct Instance {
        std::string id;
        std::string type;
        std::uint64_t anchor{};
        unsigned unit{};
        double spawned{};
        double ttl = -1;
        // Where a damage number or a flag stays: the point its unit stood at when it came.
        double x{};
        double y{};
        double distance{};
        // Its view model fields, as JSON.
        std::string fields;
    };

    // Where an instance is this frame, as UWebUIFloatingManager projects it.
    struct Projected {
        double x{};
        double y{};
        double scale{};
        double distance{};
        bool on_screen{};
        bool in_range{};
        int z{};
    };

    // The battle: everything is a function of the frame and the units' count.
    class Battle {
    public:
        explicit Battle(unsigned units) : units_(units), health_(units, max_health), flags_(units), critical_(units) {
            for (unsigned i = 0; i < units; ++i) {
                Instance bar;
                bar.id = guid();
                bar.type = "health-bar";
                bar.anchor = i + 1;
                bar.unit = i;
                bar.fields = health_fields(i);
                live_.push_back(std::move(bar));
                spawned_.push_back(live_.back().id);
            }
        }

        // The frame's events: instances that came and went, and instances whose fields changed.
        std::vector<std::string> spawned_;
        std::vector<std::string> despawned_;
        std::vector<std::string> changed_;
        std::vector<Instance> live_;

        // A frame: the battle until quiet, then nothing but movement; at quiet every unit is healed.
        void step(unsigned frame, unsigned quiet) {
            spawned_.clear();
            despawned_.clear();
            changed_.clear();
            const double t = frame * frame_s;
            // Damage numbers and flags that ran their time.
            for (auto it = live_.begin(); it != live_.end();) {
                if (it->ttl > 0 && t - it->spawned >= it->ttl) {
                    despawned_.push_back(it->id);
                    it = live_.erase(it);
                } else {
                    ++it;
                }
            }
            if (frame == quiet) {
                for (unsigned i = 0; i < units_; ++i) {
                    if (health_[i] == max_health && flags_[i].empty() && !critical_[i]) {
                        continue;
                    }
                    health_[i] = max_health;
                    flags_[i].clear();
                    if (critical_[i]) {
                        // A new bar in place of one that pulsed critical.
                        critical_[i] = false;
                        auto& old = bar(i);
                        despawned_.push_back(old.id);
                        old.id = guid();
                        old.fields = health_fields(i);
                        spawned_.push_back(old.id);
                        continue;
                    }
                    bar(i).fields = health_fields(i);
                    changed_.push_back(bar(i).id);
                }
            }
            if (frame < quiet && frame % damage_every == 0) {
                for (unsigned k = 0; k < damage_volley; ++k) {
                    hit(frame / damage_every * damage_volley + k, t);
                }
            }
            if (frame < quiet && frame % flag_every == flag_every / 2) {
                flag(frame, t);
            }
        }

        // Each instance where it is at frame t, the far ones first (as the manager sorts them; nearer ones on top).
        std::vector<Projected> project(double t) const {
            std::vector<Projected> out(live_.size());
            std::vector<std::size_t> order(live_.size());
            for (std::size_t k = 0; k < live_.size(); ++k) {
                const auto& in = live_[k];
                auto& p = out[k];
                if (in.type == "health-bar") {
                    const auto [x, y] = place(in.unit, t);
                    p.x = x;
                    p.y = y;
                    p.distance = distance(in.unit, t);
                } else {
                    p.x = in.x;
                    p.y = in.y;
                    p.distance = in.distance;
                }
                const double smallest = in.type == "damage-number" ? 0.6 : 0.5;
                p.scale = 1 + (smallest - 1) * std::clamp(p.distance / max_distance, 0.0, 1.0);
                p.on_screen = p.x >= 0 && p.x <= game_width && p.y >= 0 && p.y <= game_height;
                p.in_range = p.distance <= max_distance;
                order[k] = k;
            }
            std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                return out[a].distance > out[b].distance;
            });
            for (std::size_t z = 0; z < order.size(); ++z) {
                out[order[z]].z = int(z) + 1;
            }
            return out;
        }

        // The batch UWebUIFloatingManager pushes: every instance, far first, with its fields.
        std::string batch(double t) const {
            const auto projected = project(t);
            std::vector<std::size_t> order(live_.size());
            for (std::size_t k = 0; k < order.size(); ++k) {
                order[k] = k;
            }
            std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                return projected[a].z < projected[b].z;
            });
            std::string out = "{\"instances\":[";
            for (std::size_t n = 0; n < order.size(); ++n) {
                const auto& in = live_[order[n]];
                const auto& p = projected[order[n]];
                out += n ? ",{" : "{";
                out += "\"id\":\"" + in.id + "\",\"type\":\"" + in.type + "\",\"x\":" + number(p.x) +
                       ",\"y\":" + number(p.y) + ",\"scale\":" + number(p.scale) +
                       ",\"onScreen\":" + (p.on_screen ? "true" : "false") + ",\"behindCamera\":false" +
                       ",\"inRange\":" + (p.in_range ? "true" : "false") +
                       ",\"visible\":" + (p.on_screen && p.in_range ? "true" : "false") +
                       ",\"zOrder\":" + std::to_string(p.z) + ",\"distance\":" + number(p.distance);
                if (in.ttl > 0) {
                    out += ",\"ttl\":" + number(in.ttl) + ",\"elapsed\":" + number(t - in.spawned);
                }
                out += ",\"data\":" + in.fields + "}";
            }
            out += "],\"viewport\":{\"width\":" + std::to_string(game_width) +
                   ",\"height\":" + std::to_string(game_height) + "}}";
            return out;
        }

        // Each instance's anchor at frame t: its point in the View's CSS px, its scale and distance; hidden out of
        // range, off screen off it.
        std::vector<weft::SceneAnchor> anchors(double t) const {
            const auto projected = project(t);
            std::vector<weft::SceneAnchor> out;
            out.reserve(live_.size());
            for (std::size_t k = 0; k < live_.size(); ++k) {
                const auto& p = projected[k];
                std::uint32_t flags{};
                if (!p.in_range) {
                    flags |= weft::scene_anchor_flags::hidden;
                }
                if (!p.on_screen) {
                    flags |= weft::scene_anchor_flags::offscreen;
                }
                out.push_back({live_[k].anchor, float(p.x / ui_scale), float(p.y / ui_scale), float(p.scale),
                               float(p.distance), flags, 0});
            }
            return out;
        }

        const Instance& find(const std::string& id) const {
            const auto it = std::find_if(live_.begin(), live_.end(), [&](const Instance& in) {
                return in.id == id;
            });
            if (it == live_.end()) {
                throw std::logic_error("no instance " + id);
            }
            return *it;
        }

    private:
        unsigned units_;
        std::vector<double> health_;
        std::vector<std::vector<std::string>> flags_;
        // Whether a unit's bar has pulsed critical (HealthBar: alive under 30%).
        std::vector<bool> critical_;
        unsigned next_{};
        std::uint64_t next_anchor_ = 1u << 20;

        // A FGuid as the manager writes it (DigitsWithHyphens, upper case), the next of a counter.
        std::string guid() {
            char text[40];
            const unsigned n = ++next_;
            std::snprintf(text, sizeof text, "%08X-%04X-%04X-%04X-%012X", 0x5EF70000u + n, n & 0xFFFF, 0x4000 + n % 97,
                          0x8000 + n % 89, n * 2654435761u);
            return text;
        }

        // Where unit i stands at t, in the game's px: a circle of its own about a point of its own; one in seven
        // walks off the left or right edge now and then.
        std::array<double, 2> place(unsigned i, double t) const {
            const double cx = 220 + std::fmod(i * 397.0, game_width - 440);
            const double cy = 300 + std::fmod(i * 233.0, game_height - 520);
            const double r = i % 7 == 3 ? 900 : 40 + (i % 5) * 15;
            const double a = t * (0.4 + (i % 3) * 0.2) + i;
            return {cx + r * std::cos(a), cy + 0.4 * r * std::sin(a)};
        }

        // How far unit i is at t; one in thirteen goes past the visible range and back.
        double distance(unsigned i, double t) const {
            const double base = i % 13 == 5 ? 4400 : 1500;
            return base + (i % 13 == 5 ? 1200 : 900) * std::sin(0.25 * t + 0.9 * i);
        }

        Instance& bar(unsigned i) {
            return *std::find_if(live_.begin(), live_.end(), [&](const Instance& in) {
                return in.type == "health-bar" && in.unit == i;
            });
        }

        std::string health_fields(unsigned i) const {
            std::string flags;
            for (const auto& f : flags_[i]) {
                flags += (flags.empty() ? "" : ",") + f;
            }
            return "{\"CurrentHealth\":" + number(health_[i]) + ",\"MaxHealth\":" + number(max_health) +
                   ",\"HealthPercent\":" + number(health_[i] / max_health) + ",\"DisplayName\":\"Unit " +
                   std::to_string(i + 1) + "\",\"AbnormalFlags\":\"" + flags + "\"}";
        }

        // The battle's nth hit, on a unit: a damage number where it stands (a heal now and then), its health less or
        // more.
        void hit(unsigned n, double t) {
            const unsigned i = (n * 7) % units_;
            const auto [x, y] = place(i, t);
            const bool heal = n % 9 == 4;
            const bool critical = n % 5 == 2;
            const int amount = 5 + int((n * 37) % 90) * (critical ? 2 : 1);
            const bool modified = n % 11 == 3;
            Instance number;
            number.id = guid();
            number.type = "damage-number";
            number.anchor = next_anchor_++;
            number.unit = i;
            number.spawned = t;
            number.ttl = damage_ttl;
            number.x = x;
            number.y = y - 60;
            number.distance = distance(i, t);
            number.fields =
                "{\"Amount\":" + std::to_string(amount) +
                ",\"OriginalAmount\":" + std::to_string(modified ? amount + 12 : amount) +
                ",\"bModified\":" + (modified ? "true" : "false") + ",\"bHeal\":" + (heal ? "true" : "false") +
                ",\"bCritical\":" + (critical ? "true" : "false") + ",\"DamageType\":" + std::to_string(n % 12) +
                ",\"bOverPenetrated\":" + (n % 13 == 6 ? "true" : "false") +
                ",\"bFailedPenetrate\":" + (n % 17 == 8 ? "true" : "false") + "}";
            spawned_.push_back(number.id);
            live_.push_back(std::move(number));
            // A hit on a dead unit raises it.
            health_[i] = heal              ? std::min(max_health, health_[i] + amount * 0.3)
                         : health_[i] <= 0 ? max_health
                                           : std::max(0.0, health_[i] - amount * 0.3);
            critical_[i] = critical_[i] || (health_[i] > 0 && health_[i] < critical_health * max_health);
            bar(i).fields = health_fields(i);
            changed_.push_back(bar(i).id);
        }

        // A status flag on a unit: added, or removed if it had it.
        void flag(unsigned frame, double t) {
            static constexpr std::array<std::string_view, 3> names{"Reveal", "Unmovable", "Blind"};
            const unsigned n = frame / flag_every;
            const unsigned i = (n * 5 + 2) % units_;
            const std::string name(names[n % names.size()]);
            auto& flags = flags_[i];
            const auto had = std::find(flags.begin(), flags.end(), name);
            const bool removed = had != flags.end();
            if (removed) {
                flags.erase(had);
            } else {
                flags.push_back(name);
            }
            const auto [x, y] = place(i, t);
            Instance flag;
            flag.id = guid();
            flag.type = "abnormal-flag";
            flag.anchor = next_anchor_++;
            flag.unit = i;
            flag.spawned = t;
            flag.ttl = flag_ttl;
            flag.x = x;
            flag.y = y - 110;
            flag.distance = distance(i, t);
            flag.fields = "{\"FlagId\":\"" + name + "\",\"bRemoved\":" + (removed ? "true" : "false") + "}";
            spawned_.push_back(flag.id);
            live_.push_back(std::move(flag));
            bar(i).fields = health_fields(i);
            changed_.push_back(bar(i).id);
        }
    };

    // The native D3D11 scene pass over the game's target, its GPU time by timestamps.
    struct Pass {
        d3d::Renderer gpu;
        d3d::SceneRenderer scene;
        d3d::SceneRenderer::Slot slot;
        ComPtr<ID3D11Texture2D> target;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11Query> disjoint;
        ComPtr<ID3D11Query> begin;
        ComPtr<ID3D11Query> end;

        Pass(bool warp, weft::SceneText& text) : gpu(warp, game_width, game_height, false), scene(gpu, text) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = game_width;
            desc.Height = game_height;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            if (FAILED(gpu.device->CreateTexture2D(&desc, nullptr, &target)) ||
                FAILED(gpu.device->CreateRenderTargetView(target.Get(), nullptr, &rtv)) ||
                FAILED(gpu.device->CreateQuery(&q, &disjoint))) {
                throw std::runtime_error("the game's target");
            }
            q.Query = D3D11_QUERY_TIMESTAMP;
            if (FAILED(gpu.device->CreateQuery(&q, &begin)) || FAILED(gpu.device->CreateQuery(&q, &end))) {
                throw std::runtime_error("timestamps");
            }
        }

        // Draws the frame; the pass's CPU and GPU time.
        std::array<double, 2> draw(const weft::ViewFrame& frame, const weft::SceneAnchorTable* anchors) {
            auto* ctx = gpu.immediate.Get();
            ctx->Begin(disjoint.Get());
            ctx->End(begin.Get());
            const auto start = Clock::now();
            if (!scene.paint(slot, *frame.scene, rtv.Get(), frame.scale, game_width, game_height, std::nullopt, 1, {},
                             anchors)) {
                std::string why;
                for (const auto& [reason, count] : scene.reasons) {
                    why += " " + reason;
                }
                throw std::runtime_error("the native pass:" + why);
            }
            const double cpu = since(start);
            ctx->End(end.Get());
            ctx->End(disjoint.Get());
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
            while (ctx->GetData(disjoint.Get(), &d, sizeof d, 0) == S_FALSE) {
            }
            std::uint64_t t0{};
            std::uint64_t t1{};
            ctx->GetData(begin.Get(), &t0, sizeof t0, 0);
            ctx->GetData(end.Get(), &t1, sizeof t1, 0);
            const double gpu_ms = d.Disjoint || !d.Frequency ? 0 : double(t1 - t0) * 1000.0 / double(d.Frequency);
            return {cpu, gpu_ms};
        }

        // The target's BGRA rows.
        std::vector<std::uint8_t> read() {
            D3D11_TEXTURE2D_DESC desc{};
            target->GetDesc(&desc);
            desc.BindFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(gpu.device->CreateTexture2D(&desc, nullptr, &staging))) {
                throw std::runtime_error("readback");
            }
            gpu.immediate->CopyResource(staging.Get(), target.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(gpu.immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                throw std::runtime_error("map");
            }
            std::vector<std::uint8_t> out(std::size_t(game_width) * game_height * 4);
            for (unsigned y = 0; y < game_height; ++y) {
                std::memcpy(out.data() + std::size_t(y) * game_width * 4,
                            static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch,
                            game_width * 4);
            }
            gpu.immediate->Unmap(staging.Get(), 0);
            return out;
        }
    };

    enum class Way {
        Json,
        Anchored
    };

    struct Run {
        Way way{};
        unsigned units{};
        std::vector<double> engine;
        std::vector<double> advance;
        std::vector<double> cpu;
        std::vector<double> gpu;
        // The page's load and its first frame's advance (the start-up spike), and the VM's peak allocation (bytes).
        double load_ms{};
        double first_ms{};
        std::size_t peak_vm{};
        std::size_t published{};
        // Frames published in the quiet spell's second half (anchored: none).
        std::size_t quiet_published{};
        std::size_t nodes{};
        // Instances that went and stayed in the document: the most in the battle, and after the last frame.
        std::size_t most_left{};
        std::size_t left{};
        std::size_t batch_bytes{};
        std::size_t diagnostics{};
        std::string first_diagnostic;
        std::map<unsigned, std::vector<std::uint8_t>> shots;
    };

    std::size_t count(const weft::SceneNode& n) {
        std::size_t total = 1;
        for (const auto& c : n.children) {
            total += count(c);
        }
        return total;
    }

    // Instances in the page's overlay: those of the battle, and those that went but stay until every exit has ended
    // (the page's AnimatePresence).
    std::size_t overlaid(weft::View& view) {
        const auto children = view.script().eval(
            "(() => { const o = document.querySelectorAll('.absolute.inset-0.overflow-hidden.pointer-events-none'); "
            "return o.length === 1 ? String(o[0].children.length) : 'overlays ' + o.length; })()");
        if (children.empty() || !std::all_of(children.begin(), children.end(), [](char c) {
                return c >= '0' && c <= '9';
            })) {
            throw std::runtime_error("the page has no single overlay: " + children);
        }
        return std::stoul(children);
    }

    // Plays the battle through a View of one way: frames frames, timing the last samples, drawing each frame, keeping
    // the frames in shots, counting the instances that went and stayed (every frame when watch, else after the last).
    void play(Run& run, const std::filesystem::path& root, const std::shared_ptr<const weft::FontCatalog>& fonts,
              weft::SceneText& text, weft::SceneText& render_text, bool warp, bool watch, unsigned frames,
              unsigned samples, unsigned quiet, const std::vector<unsigned>& shots) {
        weft::View view(fonts, text, game_width / ui_scale, game_height / ui_scale, ui_scale);
        weft::ViewPage page;
        page.scripts = {{"FLOATING_TESTS.js", read(root / "examples/crpg-gallery/native/FLOATING_TESTS.js")},
                        {"mount.js", "globalThis.__ceGalleryBridge=[];__ceGallery.mount('FLOATING_TESTS'," +
                                         std::string(run.way == Way::Json ? "0" : "1") + ")"}};
        page.stylesheet = read(root / "examples/crpg-gallery/floating-utilities.css");
        page.keyframes = read(root / "examples/crpg-gallery/floating-keyframes.css");
        page.crpg_bridge = run.way == Way::Json;
        const auto loading = Clock::now();
        view.load(page);
        run.load_ms = since(loading);
        Pass pass(warp, render_text);
        Battle battle(run.units);
        auto& engine = *view.shared()->engine();
        auto last = view.shared()->frame();
        for (unsigned frame = 0; frame < frames; ++frame) {
            const double t = frame * frame_s;
            if (frame) {
                battle.step(frame, quiet);
            }
            const auto start = Clock::now();
            if (run.way == Way::Json) {
                const auto json = battle.batch(t);
                run.batch_bytes = std::max(run.batch_bytes, json.size());
                engine.emit_json("__floating_ui_batch", json);
            } else {
                // The fields that changed, those of instances that came first; then the events.
                std::string fields;
                for (const auto& id : battle.changed_) {
                    if (std::find(battle.spawned_.begin(), battle.spawned_.end(), id) == battle.spawned_.end()) {
                        fields += (fields.empty() ? "{\"" : ",\"") + id + "\":" + battle.find(id).fields;
                    }
                }
                if (!fields.empty()) {
                    engine.set_model_json("floating", fields + "}");
                }
                for (const auto& id : battle.spawned_) {
                    const auto& in = battle.find(id);
                    engine.emit_json("floating.spawn", "{\"id\":\"" + id + "\",\"type\":\"" + in.type +
                                                           "\",\"anchor\":" + std::to_string(in.anchor) +
                                                           ",\"data\":" + in.fields + "}");
                }
                for (const auto& id : battle.despawned_) {
                    engine.emit_json("floating.despawn", "{\"id\":\"" + id + "\"}");
                }
                const auto anchors = battle.anchors(t);
                if (view.shared()->write_anchors(anchors) != weft::SceneAnchorWrite::Written) {
                    throw std::runtime_error("anchors");
                }
            }
            const double engine_ms = since(start);
            const auto advanced = Clock::now();
            view.advance(t * 1000);
            const double advance_ms = since(advanced);
            if (!frame) {
                run.first_ms = advance_ms;
            }
            const auto published = view.shared()->frame();
            if (published != last) {
                ++run.published;
                if (frame >= quiet + (frames - quiet) / 2) {
                    ++run.quiet_published;
                }
            }
            last = published;
            const auto& anchors = published->anchors->take(weft::SceneAnchorChannel::Reader::Render);
            const auto [cpu, gpu] = pass.draw(*published, run.way == Way::Anchored ? &anchors : nullptr);
            if (frame >= frames - samples) {
                run.engine.push_back(engine_ms);
                run.advance.push_back(advance_ms);
                run.cpu.push_back(cpu);
                run.gpu.push_back(gpu);
            }
            if (std::find(shots.begin(), shots.end(), frame) != shots.end()) {
                run.shots[frame] = pass.read();
            }
            for (const auto& d : view.take_diagnostics()) {
                if (!run.diagnostics++) {
                    run.first_diagnostic = d.message;
                }
            }
            if (watch || frame + 1 == frames) {
                const auto instances = overlaid(view);
                if (instances < battle.live_.size()) {
                    throw std::runtime_error("instances missing from the overlay");
                }
                const auto left = instances - battle.live_.size();
                if (frame < quiet) {
                    run.most_left = std::max(run.most_left, left);
                }
                run.left = left;
            }
        }
        run.nodes = count(view.scene().root);
        run.peak_vm = view.script().stats().peak_vm_allocated_bytes;
    }

    // How two drawings of a frame differ: pixels that differ, those more than alike_levels apart (apart), and by how
    // much at most in a channel; and of the regions those apart pixels make (within alike_merge px of each other), the
    // largest side of a region's box (px), and over the box grown by alike_margin px, the largest shift of a region's
    // ink centroid between the drawings (px, by alpha) and the largest difference of its ink in a channel (a share of
    // the larger coverage, its alpha).
    struct Difference {
        std::size_t pixels{};
        std::size_t apart{};
        int max{};
        int region{};
        double shift{};
        double ink{};
    };

    Difference compare(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b,
                       std::vector<std::uint8_t>* image) {
        Difference d;
        if (image) {
            image->assign(a.size(), 0);
        }
        constexpr int w = int(game_width);
        constexpr int h = int(game_height);
        std::vector<std::uint8_t> apart(std::size_t(w) * h);
        for (std::size_t i = 0; i < a.size(); i += 4) {
            int most{};
            for (unsigned c = 0; c < 4; ++c) {
                most = std::max(most, std::abs(int(a[i + c]) - int(b[i + c])));
            }
            if (most) {
                ++d.pixels;
                d.max = std::max(d.max, most);
                if (most > alike_levels) {
                    ++d.apart;
                    apart[i / 4] = 1;
                }
                if (image) {
                    (*image)[i] = (*image)[i + 1] = (*image)[i + 2] = std::uint8_t(std::min(255, most * 8));
                    (*image)[i + 3] = 255;
                }
            }
        }
        std::vector<std::uint8_t> seen(apart.size());
        std::vector<std::array<int, 2>> stack;
        for (int y0 = 0; y0 < h; ++y0) {
            for (int x0 = 0; x0 < w; ++x0) {
                if (!apart[std::size_t(y0) * w + x0] || seen[std::size_t(y0) * w + x0]) {
                    continue;
                }
                // The region: its box.
                std::array<int, 4> box{x0, y0, x0, y0};
                seen[std::size_t(y0) * w + x0] = 1;
                stack.push_back({x0, y0});
                while (!stack.empty()) {
                    const auto [x, y] = stack.back();
                    stack.pop_back();
                    box = {std::min(box[0], x), std::min(box[1], y), std::max(box[2], x), std::max(box[3], y)};
                    for (int dy = -alike_merge; dy <= alike_merge; ++dy) {
                        for (int dx = -alike_merge; dx <= alike_merge; ++dx) {
                            const int px = x + dx;
                            const int py = y + dy;
                            const auto k = std::size_t(py) * w + px;
                            if (px >= 0 && py >= 0 && px < w && py < h && apart[k] && !seen[k]) {
                                seen[k] = 1;
                                stack.push_back({px, py});
                            }
                        }
                    }
                }
                d.region = std::max(d.region, std::max(box[2] - box[0], box[3] - box[1]) + 1);
                // Its ink in both drawings, around it.
                std::array<double, 4> ink_a{};
                std::array<double, 4> ink_b{};
                std::array<double, 2> at_a{};
                std::array<double, 2> at_b{};
                for (int y = std::max(0, box[1] - alike_margin); y <= std::min(h - 1, box[3] + alike_margin); ++y) {
                    for (int x = std::max(0, box[0] - alike_margin); x <= std::min(w - 1, box[2] + alike_margin); ++x) {
                        const auto i = (std::size_t(y) * w + x) * 4;
                        for (unsigned c = 0; c < 4; ++c) {
                            ink_a[c] += a[i + c];
                            ink_b[c] += b[i + c];
                        }
                        at_a = {at_a[0] + double(a[i + 3]) * x, at_a[1] + double(a[i + 3]) * y};
                        at_b = {at_b[0] + double(b[i + 3]) * x, at_b[1] + double(b[i + 3]) * y};
                    }
                }
                if (ink_a[3] > 0 && ink_b[3] > 0) {
                    d.shift = std::max({d.shift, std::abs(at_a[0] / ink_a[3] - at_b[0] / ink_b[3]),
                                        std::abs(at_a[1] / ink_a[3] - at_b[1] / ink_b[3])});
                } else if (ink_a[3] != ink_b[3]) {
                    d.shift = std::numeric_limits<double>::infinity();
                }
                // Premultiplied channels are at most alpha: each one's difference as a share of the coverage.
                const double coverage = std::max(ink_a[3], ink_b[3]);
                for (unsigned c = 0; c < 4 && coverage > 0; ++c) {
                    d.ink = std::max(d.ink, std::abs(ink_a[c] - ink_b[c]) / coverage);
                }
            }
        }
        return d;
    }

    // BGRA rows as RGBA, over a dark ground for the eye (the target is transparent outside the UI).
    std::vector<std::uint8_t> visible(const std::vector<std::uint8_t>& bgra) {
        std::vector<std::uint8_t> out(bgra.size());
        for (std::size_t i = 0; i < bgra.size(); i += 4) {
            const int a = bgra[i + 3];
            const auto over = [&](int c, int ground) {
                return std::uint8_t(c + ground * (255 - a) / 255);
            };
            out[i] = over(bgra[i + 2], 38);
            out[i + 1] = over(bgra[i + 1], 44);
            out[i + 2] = over(bgra[i], 52);
            out[i + 3] = 255;
        }
        return out;
    }
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3 || (std::string_view(argv[1]) != "--quick" && std::string_view(argv[1]) != "--full")) {
            std::cerr << "usage: weft_floating_benchmark --quick|--full <folder>\n";
            return 2;
        }
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
            throw std::runtime_error("COM");
        }
        const bool full = std::string_view(argv[1]) == "--full";
        const std::filesystem::path root = WEFT_SOURCE_DIR;
        const std::filesystem::path folder = argv[2];
        std::filesystem::create_directories(folder);
        auto fonts = std::make_shared<const weft::FontCatalog>(weft::platform::win32::system_font_catalog());
        weft::SceneText text(*fonts);
        weft::SceneText render_text(*fonts);
        // The battle, then the quiet spell; frames to draw both ways and compare (battle, quiet).
        const unsigned battle_frames = full ? 660 : 150;
        const unsigned quiet_frames = full ? 0 : 240;
        const unsigned frames = battle_frames + quiet_frames;
        const unsigned samples = full ? 600 : frames;
        const std::vector<unsigned> shots =
            full ? std::vector<unsigned>{120, 450} : std::vector<unsigned>{40, 96, 145, frames - 1};
        const std::vector<unsigned> levels = full ? std::vector<unsigned>{12, 50, 200} : std::vector<unsigned>{12};
        std::ostringstream out;
        out << "{\"format\":\"weft-floating-benchmark-v1\",\"build\":"
#ifdef NDEBUG
            << "\"Release\""
#else
            << "\"Debug\""
#endif
            << ",\"device\":\"" << (full ? "hardware" : "warp") << "\",\"game\":[" << game_width << ',' << game_height
            << "],\"ui_scale\":" << ui_scale << ",\"frames\":" << frames << ",\"sampled\":" << samples
            << ",\"levels\":[";
        bool alike = true;
        bool quiet_ok = true;
        bool left_ok = true;
        for (std::size_t l = 0; l < levels.size(); ++l) {
            Run json{Way::Json, levels[l]};
            Run anchored{Way::Anchored, levels[l]};
            play(json, root, fonts, text, render_text, !full, !full, frames, samples, battle_frames, shots);
            play(anchored, root, fonts, text, render_text, !full, !full, frames, samples, battle_frames, shots);
            out << (l ? "," : "") << "{\"units\":" << levels[l] << ",\"ways\":[";
            for (const auto* run : {&json, &anchored}) {
                out << (run == &json ? "" : ",") << "{\"way\":\"" << (run == &json ? "json" : "anchored")
                    << "\",\"nodes\":" << run->nodes << ",\"most_left\":" << run->most_left << ",\"left\":" << run->left
                    << ",\"published\":" << run->published << ",\"quiet_published\":" << run->quiet_published
                    << ",\"batch_bytes\":" << run->batch_bytes << ",\"diagnostics\":" << run->diagnostics
                    << ",\"load_ms\":" << run->load_ms << ",\"first_frame_ms\":" << run->first_ms
                    << ",\"peak_vm_bytes\":" << run->peak_vm << ",\"engine_ms\":" << stats(run->engine)
                    << ",\"advance_ms\":" << stats(run->advance) << ",\"render_cpu_ms\":" << stats(run->cpu)
                    << ",\"gpu_ms\":" << stats(run->gpu) << '}';
                std::cout << levels[l] << " units, " << (run == &json ? "json    " : "anchored") << ": engine p50 "
                          << std::fixed << std::setprecision(3) << percentile(run->engine, .5)
                          << " ms, advance p50 / p99 " << percentile(run->advance, .5) << " / "
                          << percentile(run->advance, .99) << " / " << percentile(run->advance, 1)
                          << " ms (max), render CPU p50 / p99 " << percentile(run->cpu, .5) << " / "
                          << percentile(run->cpu, .99) << " ms, GPU p50 " << percentile(run->gpu, .5)
                          << " ms, frames published " << run->published << " (" << run->quiet_published
                          << " late in the quiet spell), load " << run->load_ms << " ms, first frame " << run->first_ms
                          << " ms, VM peak " << double(run->peak_vm) / (1 << 20) << " MiB, " << run->nodes
                          << " nodes, instances that went stayed " << run->most_left << " at most and " << run->left
                          << " at the end\n";
                if (run->diagnostics) {
                    std::cerr << "page diagnostic: " << run->first_diagnostic << '\n';
                }
            }
            out << "],\"shots\":[";
            for (std::size_t s = 0; s < shots.size(); ++s) {
                const auto& a = json.shots.at(shots[s]);
                const auto& b = anchored.shots.at(shots[s]);
                std::vector<std::uint8_t> diff;
                const auto d = compare(a, b, &diff);
                const auto name = std::to_string(levels[l]) + "-" + std::to_string(shots[s]);
                weft::testing::write_png(folder / ("json-" + name + ".png"), game_width, game_height, visible(a));
                weft::testing::write_png(folder / ("anchored-" + name + ".png"), game_width, game_height, visible(b));
                weft::testing::write_png(folder / ("diff-" + name + ".png"), game_width, game_height, visible(diff));
                out << (s ? "," : "") << "{\"frame\":" << shots[s] << ",\"differing_pixels\":" << d.pixels
                    << ",\"apart_pixels\":" << d.apart << ",\"max_channel_difference\":" << d.max
                    << ",\"largest_region\":" << d.region << ",\"largest_shift\":" << d.shift
                    << ",\"largest_ink_difference\":" << d.ink << '}';
                std::cout << "  frame " << shots[s] << ": " << d.pixels << " pixels differ (" << d.apart
                          << " by more than " << alike_levels << "), by at most " << d.max << "; regions up to "
                          << d.region << " px, ink shifted " << d.shift << " px, " << d.ink * 100 << "% apart\n";
                alike = alike && d.region <= alike_region && d.shift <= alike_shift && d.ink <= alike_ink;
            }
            out << "]}";
            quiet_ok = quiet_ok && (full || (anchored.quiet_published == 0 && json.quiet_published > 0));
            for (const auto* run : {&json, &anchored}) {
                left_ok = left_ok && (full || (run->most_left <= most_left && run->left == 0));
            }
            if (json.diagnostics || anchored.diagnostics) {
                alike = false;
            }
        }
        out << "]}";
        std::ofstream(folder / "floating.json", std::ios::binary) << out.str();
        if (!full && (!alike || !quiet_ok || !left_ok)) {
            std::cerr << "floating benchmark failed: " << (!alike ? "the two ways draw differently or diagnose" : "")
                      << (!quiet_ok ? " the anchored way published frames in the quiet spell" : "")
                      << (!left_ok ? " instances that went stayed in the document" : "") << '\n';
            return 1;
        }
        std::cout << "PASS floating benchmark: " << levels.size() << " levels, " << frames << " frames each\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "floating benchmark failed: " << e.what() << '\n';
        return 1;
    }
}
