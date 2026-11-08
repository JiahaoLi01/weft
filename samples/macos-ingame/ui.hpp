#pragma once

// The game side of the in-game sample, through the public C header only (include/weft/weft.h): one runtime whose Views
// are the world's panels (pages.hpp), where each panel stands in the world, the ray that finds the panel and the point
// on its page under the pointer, and the game state the pages change and show: the reactor's output, which the
// terminal sets, and the status the board shows once a second.
#include "pages.hpp"

#include <weft/weft.h>

#include <simd/simd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace ingame {
    struct Failure : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    inline void check(bool ok, const std::string& what) {
        if (!ok) {
            throw Failure(what);
        }
    }

    inline void check(weft_result result, const std::string& what) {
        if (result != WEFT_OK) {
            throw Failure(what + ": " + weft_result_string(result));
        }
    }

    // A number of a MessagePack payload (weft_message): a fixint, an unsigned int of up to 32 bits, or a float.
    inline bool msgpack_number(const void* payload, std::size_t size, double& out) {
        const auto* p = static_cast<const std::uint8_t*>(payload);
        if (size == 1 && (p[0] < 0x80 || p[0] >= 0xe0)) {
            out = p[0] < 0x80 ? p[0] : int(p[0]) - 256;
            return true;
        }
        std::uint64_t bits{};
        for (std::size_t i = 1; i < size; ++i) {
            bits = bits << 8 | p[i];
        }
        if (size == 2 && p[0] == 0xcc) {
            out = double(bits);
        } else if (size == 3 && p[0] == 0xcd) {
            out = double(bits);
        } else if (size == 5 && p[0] == 0xce) {
            out = double(bits);
        } else if (size == 5 && p[0] == 0xca) {
            float f;
            const auto u = std::uint32_t(bits);
            std::memcpy(&f, &u, sizeof f);
            out = f;
        } else if (size == 9 && p[0] == 0xcb) {
            std::memcpy(&out, &bits, sizeof out);
        } else {
            return false;
        }
        return true;
    }

    // Where a frame's CPU time went (ms), for the sample's profile: the status event the game sent (when it did),
    // each page's advance (with its messages and diagnostics), each page's prepare and export encode (when it drew),
    // the commit of the pages' command buffer, the world's encoding and its commit.
    struct Phases {
        static constexpr std::size_t max_panels = 4;
        double status{};
        std::array<double, max_panels> advance{};
        std::array<double, max_panels> prepare{};
        std::array<double, max_panels> encode{};
        double pages_commit{};
        double world{};
        double commit{};
        bool second{};

        using Clock = std::chrono::steady_clock;

        static double since(Clock::time_point start) {
            return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        }
    };

    // A panel: its View (logical px at a device scale: the texture is that many times larger), and its place in the
    // world: its center, and half its width and height along its right and up directions (m). It faces along
    // cross(right, up).
    struct Panel {
        const char* name{};
        weft_view_handle view{};
        float width{};
        float height{};
        float scale{};
        simd_float3 center{};
        simd_float3 right{};
        simd_float3 up{};

        unsigned pixel_width() const {
            return unsigned(width * scale);
        }

        unsigned pixel_height() const {
            return unsigned(height * scale);
        }

        simd_float3 normal() const {
            return simd_normalize(simd_cross(right, up));
        }
    };

    // Where a ray meets a panel's front: the panel, the point on its page (logical px from its top left, possibly
    // outside it when the plane is all that is asked for), and how far along the ray.
    struct Pick {
        int panel = -1;
        float x{};
        float y{};
        float distance{};
    };

    struct Ui {
        weft_runtime* runtime{};
        std::vector<Panel> panels;
        // The panels' indices.
        static constexpr int terminal_panel = 0;
        static constexpr int status_panel = 1;
        static constexpr int sign_panel = 2;
        // The game: the reactor's output (0-100) as the terminal set it, how often it did, the core's temperature over
        // the last sixteen seconds, and the second the board last showed.
        int output = 40;
        unsigned output_messages{};
        std::vector<float> temps = std::vector<float>(16, 30);
        long shown_second = -1;
        unsigned diagnostics{};
        bool animated_status{};
        // The status event every frame instead of once a second, its time to the millisecond: a page whose text
        // changes every frame, to profile.
        bool status_every_frame{};
        long frames{};

        Ui() {
            check(weft_runtime_create(nullptr, &runtime), "runtime");
            // The terminal faces the camera from its left, the board from its right; the sign stands far behind.
            add(terminal, terminal_width, terminal_height, 2, {-1.95f, 1.45f, 0.9f}, 0.42f, 1.6f);
            add(status, 420, 260, 2, {2.05f, 1.6f, 0.5f}, -0.48f, 1.45f);
            add(sign, 1024, 256, 2, {0.f, 3.9f, -9.f}, 0.f, 8.f);
        }

        ~Ui() {
            for (const auto& p : panels) {
                weft_view_destroy(runtime, p.view);
            }
            weft_runtime_destroy(runtime);
        }

        Ui(const Ui&) = delete;
        Ui& operator=(const Ui&) = delete;

        // A panel of page, width x height px at scale, centered at center, turned yaw (rad) about +y from facing +z,
        // meters wide.
        void add(const PageSource& page, float width, float height, float scale, simd_float3 center, float yaw,
                 float meters) {
            Panel p;
            p.name = page.name;
            p.width = width;
            p.height = height;
            p.scale = scale;
            const weft_view_desc desc{sizeof(desc), 0, width, height, scale, 0};
            check(weft_view_create(runtime, &desc, &p.view), std::string("the View of ") + page.name);
            const weft_script script{page.name, std::strlen(page.name), page.script, std::strlen(page.script)};
            const weft_page_desc d{sizeof(d),
                                   0,
                                   &script,
                                   1,
                                   0,
                                   page.stylesheet,
                                   std::strlen(page.stylesheet),
                                   page.keyframes,
                                   page.keyframes ? std::strlen(page.keyframes) : 0,
                                   nullptr,
                                   0,
                                   nullptr,
                                   0};
            check(weft_view_load(runtime, p.view, &d), std::string("the page ") + page.name);
            p.center = center;
            p.right = simd_make_float3(std::cos(yaw), 0, -std::sin(yaw)) * (meters / 2);
            p.up = simd_make_float3(0, 1, 0) * (meters * height / width / 2);
            panels.push_back(p);
        }

        // The pages run to t (s): the board gets the game's status once a second, and the pages' messages reach the
        // game.
        void advance(double t, Phases* phases = nullptr) {
            const long second = long(t);
            const auto started = Phases::Clock::now();
            ++frames;
            if (second != shown_second || status_every_frame) {
                shown_second = second;
                const float target = 280 + float(output) * 9;
                temps.erase(temps.begin());
                const float noise = float((second * 7919) % 13) - 6;
                temps.push_back(std::clamp((target - 280) / 9 + noise, 4.f, 100.f));
                char time[16];
                if (status_every_frame) {
                    std::snprintf(time, sizeof time, "%02ld:%02ld.%03ld", second / 60 % 60, second % 60,
                                  long(t * 1000) % 1000);
                } else {
                    std::snprintf(time, sizeof time, "%02ld:%02ld:%02ld", second / 3600, second / 60 % 60,
                                  second % 60);
                }
                std::string json = "{\"time\":\"" + std::string(time) + "\",\"temps\":[";
                for (std::size_t i = 0; i < temps.size(); ++i) {
                    json += (i ? "," : "") + std::to_string(int(temps[i]));
                }
                const char* core = output >= 90 ? "CRITICAL" : output >= 70 ? "HIGH" : "STABLE";
                json += "],\"output\":" + std::to_string(output) + ",\"core\":\"" + core +
                        "\",\"temp\":" + std::to_string(int(target)) + "}";
                emit(status_panel, "status", json);
                if (phases) {
                    phases->second = true;
                    phases->status = Phases::since(started);
                }
            }
            for (std::size_t i = 0; i < panels.size(); ++i) {
                const auto& p = panels[i];
                const auto begin = Phases::Clock::now();
                check(weft_view_advance(runtime, p.view, t), "advance");
                check(weft_view_drain_messages(runtime, p.view, &Ui::message, this), "messages");
                check(weft_view_drain_diagnostics(
                          runtime, p.view,
                          [](void* user, weft_view_handle, const weft_diagnostic* d) {
                              std::fprintf(stderr, "page: %.*s\n", int(d->message_size), d->message);
                              ++static_cast<Ui*>(user)->diagnostics;
                          },
                          this, nullptr),
                      "diagnostics");
                if (phases) {
                    phases->advance[i] = Phases::since(begin);
                }
            }
        }

        void emit(int panel, const std::string& name, const std::string& json) {
            check(weft_view_emit(runtime, panels[std::size_t(panel)].view, name.data(), name.size(), WEFT_VALUE_JSON,
                                 json.data(), json.size(), 0),
                  "emit " + name);
        }

        // The board's scanning bar, which changes the page every frame.
        void animate_status(bool on) {
            animated_status = on;
            emit(status_panel, "status.animate", on ? "true" : "false");
        }

        static void message(void* user, weft_view_handle, const weft_message* m) {
            auto& ui = *static_cast<Ui*>(user);
            const std::string name(m->name, m->name_size);
            double value{};
            if (m->kind == WEFT_MESSAGE_SEND && name == "reactor.output" &&
                msgpack_number(m->payload, m->payload_size, value)) {
                ui.output = std::clamp(int(value), 0, 100);
                ++ui.output_messages;
            }
        }

        // The point where the ray (origin, unit direction) meets panel i's plane from its front, at any distance
        // ahead; none when the ray runs along it, away from it or at its back.
        bool plane(int i, simd_float3 origin, simd_float3 direction, Pick& out) const {
            const auto& p = panels[std::size_t(i)];
            const simd_float3 n = p.normal();
            const float facing = simd_dot(direction, n);
            if (facing > -1e-4f) {
                return false;
            }
            const float distance = simd_dot(p.center - origin, n) / facing;
            if (distance <= 0) {
                return false;
            }
            const simd_float3 at = origin + direction * distance - p.center;
            const float u = simd_dot(at, p.right) / simd_length_squared(p.right);
            const float v = simd_dot(at, p.up) / simd_length_squared(p.up);
            out = {i, (u + 1) / 2 * p.width, (1 - v) / 2 * p.height, distance};
            return true;
        }

        // The nearest panel the ray meets inside its page.
        Pick pick(simd_float3 origin, simd_float3 direction) const {
            Pick best;
            for (int i = 0; i < int(panels.size()); ++i) {
                Pick hit;
                const auto& p = panels[std::size_t(i)];
                if (plane(i, origin, direction, hit) && hit.x >= 0 && hit.y >= 0 && hit.x < p.width &&
                    hit.y < p.height && (best.panel < 0 || hit.distance < best.distance)) {
                    best = hit;
                }
            }
            return best;
        }

        void pointer(int panel, weft_pointer_kind kind, float x, float y, std::uint32_t buttons, float wheel_x = 0,
                     float wheel_y = 0) {
            const weft_pointer_event e{sizeof(e), std::uint32_t(kind), x, y, buttons, 0, wheel_x, wheel_y};
            check(weft_view_pointer(runtime, panels[std::size_t(panel)].view, &e, nullptr), "pointer");
        }

        weft_hit hit(int panel, float x, float y) const {
            weft_hit out{};
            out.struct_size = sizeof(out);
            check(weft_view_hit_test(runtime, panels[std::size_t(panel)].view, x, y, &out), "hit test");
            return out;
        }

        // The core: its color (linear, scaled by its glow) and how fast it turns (rad/s), from the reactor's output:
        // blue when low, teal at half, orange when high.
        simd_float3 core_color() const {
            const float k = float(output) / 100;
            const simd_float3 cool = simd_make_float3(0.03f, 0.18f, 1.0f);
            const simd_float3 mid = simd_make_float3(0.02f, 0.75f, 0.55f);
            const simd_float3 hot = simd_make_float3(1.0f, 0.22f, 0.02f);
            const simd_float3 hue = k < 0.5f ? simd_mix(cool, mid, simd_make_float3(k, k, k) * 2)
                                             : simd_mix(mid, hot, simd_make_float3(k, k, k) * 2 - 1);
            return hue * (0.5f + 1.1f * k);
        }

        float core_speed() const {
            return 0.3f + 2.2f * float(output) / 100;
        }
    };
} // namespace ingame
