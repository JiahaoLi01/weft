#pragma once

// The game side of the Apple host samples (samples/macos-host, and samples/ios-host, which includes it), through the
// public C header only (include/weft/weft.h): a runtime and a View that runs the HUD page of examples/engine-host, as
// samples/engine-host drives it on Windows. The engine's units walk circles in the world (the View's logical px); it
// writes their screen positions as anchors, their health to the view model "health" and damage to hud.damage events,
// registers the page's sync function loc.get, and answers the page's hud.endTurn call.
#include <weft/weft.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace demo {
    constexpr unsigned units = 12;
    // The minimap's texels: the world a tenth as wide.
    constexpr unsigned map_width = 128;
    constexpr unsigned map_height = 72;

    struct Failure : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    inline void check(bool ok, const std::string& what) {
        if (!ok) {
            throw Failure(what);
        }
    }

    inline void check(weft_result result, const char* what) {
        if (result != WEFT_OK) {
            throw Failure(std::string(what) + ": " + weft_result_string(result));
        }
    }

    inline std::string read(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        check(bool(in), "cannot read " + path);
        return {std::istreambuf_iterator<char>(in), {}};
    }

    // A page as hosts load it: its scripts in order, stylesheet, keyframes and base (where its stylesheet lies), the
    // WEFT_PAGE_* flags and the root's selector (empty: #root). hud: the engine-host HUD, which the game drives (units,
    // health, damage, minimap); other pages run on their own over a plain backdrop.
    struct Page {
        std::string name;
        std::vector<std::pair<std::string, std::string>> scripts;
        std::string stylesheet;
        std::string keyframes;
        std::string base;
        std::uint32_t flags{};
        std::string root;
        bool hud{};
    };

    // The HUD page of examples/engine-host: hud.js, hud.css and hud-keyframes.css.
    inline Page hud_page(const std::string& folder) {
        Page page;
        page.name = "HUD";
        page.scripts.emplace_back("hud.js", read(folder + "/hud.js"));
        page.stylesheet = read(folder + "/hud.css");
        page.keyframes = read(folder + "/hud-keyframes.css");
        page.base = folder + "/hud.css";
        page.hud = true;
        return page;
    }

    // The engine: its game state and the View it drives.
    struct Engine {
        weft_runtime* runtime{};
        weft_view_handle view{};
        // The world's size: the View's logical px.
        float width{};
        float height{};
        bool ready{};
        unsigned turn = 1;
        unsigned answered{};
        unsigned diagnostics{};
        std::array<float, units> health{};
        double last_damage{};
        unsigned next_victim{};

        bool hud{};
        std::string page_name;

        Engine(const Page& page, float w, float h, float scale) : width(w), height(h) {
            check(weft_runtime_create(nullptr, &runtime), "runtime");
            const weft_view_desc desc{sizeof(desc), 0, width, height, scale, 0};
            check(weft_view_create(runtime, &desc, &view), "view");
            // A sync function the page calls in the middle of its script: the engine's localization.
            check(weft_view_register_function(
                      runtime, view, "loc.get", 7,
                      [](void*, const void*, size_t, weft_value_writer* result) {
                          static const char title[] = "\"Battle of Weft (Metal)\"";
                          return weft_value_writer_set(result, WEFT_VALUE_JSON, title, sizeof title - 1);
                      },
                      nullptr),
                  "register loc.get");
            load(page);
        }

        // Replaces the View's page (the runtime and its fonts stay, as the renderer needs). The HUD's units spawn with
        // full health.
        void load(const Page& page) {
            std::vector<weft_script> scripts;
            for (const auto& [name, source] : page.scripts) {
                scripts.push_back({name.data(), name.size(), source.data(), source.size()});
            }
            const weft_page_desc desc{sizeof(desc),
                                      page.flags,
                                      scripts.data(),
                                      std::uint32_t(scripts.size()),
                                      0,
                                      page.stylesheet.data(),
                                      page.stylesheet.size(),
                                      page.keyframes.data(),
                                      page.keyframes.size(),
                                      page.root.empty() ? nullptr : page.root.data(),
                                      page.root.size(),
                                      page.base.data(),
                                      page.base.size()};
            check(weft_view_load(runtime, view, &desc), ("load the page " + page.name).c_str());
            hud = page.hud;
            page_name = page.name;
            ready = false;
            health.fill(1);
            if (hud) {
                for (unsigned i = 0; i < units; ++i) {
                    const auto spawn =
                        "{\"id\":" + std::to_string(i + 1) + ",\"name\":\"Unit " + std::to_string(i + 1) + "\"}";
                    check(weft_view_emit(runtime, view, "hud.spawn", 9, WEFT_VALUE_JSON, spawn.data(), spawn.size(), 0),
                          "spawn");
                }
            }
        }

        ~Engine() {
            weft_view_destroy(runtime, view);
            weft_runtime_destroy(runtime);
        }

        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;

        // Where unit i stands at time t (s), in the world's px: a circle of its own.
        std::array<float, 2> place(unsigned i, double t) const {
            const float span_x = std::max(width - 300, 1.f);
            const float span_y = std::max(height - 300, 1.f);
            const float cx = 120 + std::fmod(float(i * 197), span_x);
            const float cy = 120 + std::fmod(float(i * 113), span_y);
            const float r = 30 + float(i % 4) * 12;
            const float a = float(t) * (0.8f + float(i % 3) * 0.3f) + float(i);
            return {cx + r * std::cos(a), cy + r * std::sin(a)};
        }

        void resize(float w, float h, float scale) {
            width = w;
            height = h;
            check(weft_view_resize(runtime, view, w, h, scale), "resize");
        }

        // The display's safe area in the View's px, which the page reads as env(safe-area-inset-*): a host whose View
        // covers the whole display (samples/ios-host) gives it; one that keeps the View clear of the edges leaves 0.
        void safe_area(float top, float right, float bottom, float left) {
            check(weft_view_set_safe_area(runtime, view, top, right, bottom, left), "safe area");
        }

        // Unit i takes amount damage: the view model's health and a damage number; a unit near death heals.
        void damage(unsigned i, unsigned amount) {
            health[i] = health[i] > 0.2f ? health[i] - float(amount) / 100 : 1;
            const auto fields = "{\"" + std::to_string(i + 1) + "\":" + std::to_string(health[i]) + "}";
            check(weft_view_set_model(runtime, view, "health", 6, WEFT_VALUE_JSON, fields.data(), fields.size()),
                  "health");
            const auto event = "{\"id\":" + std::to_string(i + 1) + ",\"amount\":" + std::to_string(amount) + "}";
            check(weft_view_emit(runtime, view, "hud.damage", 10, WEFT_VALUE_JSON, event.data(), event.size(), 0),
                  "damage");
        }

        // The unit within reach of a point of the world, if any.
        int unit_at(float x, float y, double t) const {
            int best = -1;
            float nearest = 28 * 28;
            for (unsigned i = 0; i < units; ++i) {
                const auto [ux, uy] = place(i, t);
                const float d = (ux - x) * (ux - x) + (uy - y) * (uy - y);
                if (d < nearest) {
                    nearest = d;
                    best = int(i);
                }
            }
            return best;
        }

        // One frame of the game at time t: the units move (anchors), now and then one takes damage; then the View
        // advances and the engine answers the page.
        void frame(double t) {
            if (!hud) {
                advance(t);
                return;
            }
            // Each nameplate stands over its unit's head: the anchor is above the unit's disc.
            std::vector<weft_anchor> anchors(units);
            for (unsigned i = 0; i < units; ++i) {
                const auto [x, y] = place(i, t);
                anchors[i] = {i + 1, x, y - 20, 1, y, 0, 0};
            }
            check(weft_view_write_anchors(runtime, view, anchors.data(), anchors.size()), "anchors");
            if (t - last_damage > 0.6) {
                last_damage = t;
                damage(next_victim, 15);
                next_victim = (next_victim + 5) % units;
            }
            advance(t);
        }

        // The View runs to t; the engine answers the page and prints what it says.
        void advance(double t) {
            check(weft_view_advance(runtime, view, t), "advance");
            check(weft_view_drain_messages(runtime, view, &Engine::message, this), "messages");
            check(weft_view_drain_diagnostics(
                      runtime, view,
                      [](void* user, weft_view_handle, const weft_diagnostic* d) {
                          std::fprintf(stderr, "page: %.*s\n", int(d->message_size), d->message);
                          ++static_cast<Engine*>(user)->diagnostics;
                      },
                      this, nullptr),
                  "diagnostics");
        }

        // A pointer event in the View's logical px.
        void pointer(weft_pointer_kind kind, float x, float y, std::uint32_t buttons, float wheel_x = 0,
                     float wheel_y = 0) {
            const weft_pointer_event e{sizeof(e), std::uint32_t(kind), x, y, buttons, 0, wheel_x, wheel_y};
            check(weft_view_pointer(runtime, view, &e, nullptr), "pointer");
        }

        // What the page has under a point, and its cursor.
        weft_hit hit(float x, float y) const {
            weft_hit out{};
            out.struct_size = sizeof(out);
            check(weft_view_hit_test(runtime, view, x, y, &out), "hit test");
            return out;
        }

        // The minimap at time t, linear premultiplied RGBA8 rows top down: a dark sea, each unit a dot from green to
        // red as it loses health.
        std::vector<std::uint8_t> minimap(double t) const {
            std::vector<std::uint8_t> texels(std::size_t(map_width) * map_height * 4);
            for (std::size_t i = 0; i < texels.size(); i += 4) {
                texels[i] = 2;
                texels[i + 1] = 6;
                texels[i + 2] = 14;
                texels[i + 3] = 255;
            }
            for (unsigned i = 0; i < units; ++i) {
                const auto [x, y] = place(i, t);
                const int cx = int(x / width * float(map_width));
                const int cy = int(y / height * float(map_height));
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int px = cx + dx;
                        const int py = cy + dy;
                        if (px < 0 || py < 0 || px >= int(map_width) || py >= int(map_height)) {
                            continue;
                        }
                        auto* p = &texels[(std::size_t(py) * map_width + std::size_t(px)) * 4];
                        p[0] = std::uint8_t(255 * (1 - health[i]));
                        p[1] = std::uint8_t(255 * health[i]);
                        p[2] = 0;
                        p[3] = 255;
                    }
                }
            }
            return texels;
        }

        static void message(void* user, weft_view_handle view, const weft_message* m) {
            auto& engine = *static_cast<Engine*>(user);
            const std::string name(m->name, m->name_size);
            if (m->kind == WEFT_MESSAGE_SEND && name == "hud.ready") {
                engine.ready = true;
            } else if (m->kind == WEFT_MESSAGE_CALL && name == "hud.endTurn") {
                ++engine.turn;
                const auto answer = "{\"turn\":" + std::to_string(engine.turn) + "}";
                check(weft_view_complete_call(engine.runtime, view, m->call_id, 1, WEFT_VALUE_JSON, answer.data(),
                                              answer.size()),
                      "complete the call");
                ++engine.answered;
                std::printf("engine: turn %u\n", engine.turn);
            }
        }
    };
} // namespace demo
