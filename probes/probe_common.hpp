#pragma once

// Shared by the D3D11 probe translation units: includes, the Experiment renderer with the color
// fixture's checks, fixtures and report helpers. Code lives in namespace probes at global scope, so
// unqualified names resolve as they did in the former single-file probe.
#define NOMINMAX
#include "layout/auto_page.hpp"
#include "script/script_platform.hpp"
#include "layout/flow_page.hpp"
#include "scene/scene.hpp"
#include "scene/scene_native.hpp"
#include "core/owner_executor.hpp"
#include "render/resource_lifetime.hpp"
#include "text/font.hpp"
#include "layout/page.hpp"
#include "layout/positioned_page.hpp"
#include "input/mouse_router.hpp"
#include "weft/render_backend_d3d11.h"
#include "render/backends/d3d11/host.hpp"
#include "render/backends/d3d11/two_frame_pipeline.hpp"
#include "render/srgb.hpp"
#include "constants/common.hpp"
#include "probe_constants.hpp"
#include "gallery_constants.hpp"
#include "../tests/test_constants/scene.hpp"
#include "../tests/test_constants/support.hpp"
#include "../tests/support/check.hpp"
#include "../tests/support/files.hpp"
#include "../tests/support/frame_rate.hpp"
#include "../tests/support/host_time.hpp"
#include "../tests/support/json.hpp"
#include "../tests/support/owner_trace.hpp"
#include "../tests/support/png.hpp"
#include "../tests/support/scene_fonts.hpp"
#include <windowsx.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <d3d11sdklayers.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <map>
#include <span>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

namespace probes {
    using Microsoft::WRL::ComPtr;
    using namespace weft::render;
    using namespace weft::render::d3d11;
    constexpr UINT width = weft::probe_constants::color_fixture_width;
    constexpr UINT height = weft::probe_constants::color_fixture_height;

    using weft::testing::check_hresult;
    using weft::testing::format_host_time;
    using weft::testing::json_string;
    using weft::testing::OwnerTrace;
    using weft::testing::read_file;
    using weft::testing::require;
    using weft::testing::TraceControl;
    using weft::testing::WindowFrameRate;

    inline void lifetime(LifetimeResult r, const char* operation) {
        require(r == LifetimeResult::Ok, operation);
    }

    inline float decode(float c) {
        return weft::render::srgb_to_linear(c);
    }

    using Pixel = std::array<std::uint8_t, 4>;
    using Color = std::array<float, 4>;
    using Rect = weft::geometry::Rect;

    inline FramePacket scene() {
        FramePacket p;
        p.assets.push_back(std::make_shared<const Asset>(Asset{1, 1, Encoding::Coverage, {128}}));
        // Fully transparent blue must not pollute filtered opaque red.
        p.assets.push_back(
            std::make_shared<const Asset>(Asset{2, 1, Encoding::SrgbStraight, {255, 0, 0, 255, 0, 0, 255, 0}}));
        weft::Font font(WEFT_SOURCE_DIR "/third_party/ahem/Ahem.ttf");
        const auto shaped = font.shape("X", 24);
        const auto glyph = font.rasterize(shaped.glyphs[0].glyph_id, 24);
        p.assets.push_back(
            std::make_shared<const Asset>(Asset{glyph.width, glyph.height, Encoding::Coverage, glyph.coverage}));
        p.draws.push_back(
            {{0, 0, weft::probe_constants::color_fixture_width, weft::probe_constants::color_fixture_height},
             {0, 0, 0, 1}});
        p.draws.push_back({{0, 0, 64, 64}, {1, 1, 1, 0.5f}});
        p.draws.push_back({{64, 0, 64, 64}, {1, 0, 0, 0.5f}});
        p.draws.push_back({{64, 0, 64, 64}, {0, 0, 1, 0.5f}});
        p.draws.push_back({{128, 0, 64, 64}, {128 / 255.0f, 128 / 255.0f, 128 / 255.0f, 1}});
        p.draws.push_back({{192, 0, 64, 64}, {1, 1, 1, 1}, 0});
        p.draws.push_back({{0, 64, 64, 64}, {1, 1, 1, 1}, -1, true, false, 0.5f});
        p.draws.push_back({{64, 64, 64, 64}, {1, 1, 1, 1}, 1, false, false, 1, {0.5f, 0.5f, 0, 0}});
        p.draws.push_back({{148, 84, 24, 24}, {1, 1, 1, 1}, 2});
        p.draws.push_back({{192, 64, 64, 64}, {1, 0, 0, 1}, -1, false, true});
        return p;
    }

    struct SdfScene {
        FramePacket packet;
        std::array<Rect, 3> squares{{{16.25f, 14.25f, 20, 20}, {56.25f, 14.25f, 40, 40}, {128.25f, 14.25f, 60, 60}}};
        Rect stroked{212.25f, 14.25f, 20, 20};
        std::size_t generated_glyphs{};
        std::size_t distance_bytes{};
    };

    inline SdfScene sdf_scene() {
        SdfScene scene;
        auto atlas = std::make_shared<Asset>(Asset{
            weft::probe_constants::sdf_atlas_width, weft::probe_constants::sdf_atlas_height, Encoding::SignedDistance,
            std::vector<std::uint8_t>(weft::probe_constants::sdf_atlas_width * weft::probe_constants::sdf_atlas_height),
            weft::constants::text::sdf_default_spread});

        struct Cell {
            weft::SdfBitmap bitmap;
            UINT x{};
            UINT y{};
        };

        const auto pack = [&](weft::SdfBitmap bitmap, UINT x, UINT y) {
            require(x + bitmap.width + 1 < atlas->width && y + bitmap.height + 1 < atlas->height,
                    "fixed probe atlas capacity");
            for (UINT row = 0; row < bitmap.height; ++row) {
                std::copy_n(bitmap.distances.data() + static_cast<std::size_t>(row) * bitmap.width, bitmap.width,
                            atlas->bytes.data() + static_cast<std::size_t>(y + row) * atlas->width + x);
            }
            ++scene.generated_glyphs;
            scene.distance_bytes += bitmap.distances.size();
            return Cell{std::move(bitmap), x, y};
        };
        weft::Font ahem(WEFT_SOURCE_DIR "/third_party/ahem/Ahem.ttf");
        const auto shape = ahem.shape("X", weft::constants::text::sdf_default_base_ppem);
        const auto square =
            pack(ahem.rasterize_sdf(shape.glyphs[0].glyph_id, weft::constants::text::sdf_default_base_ppem,
                                    weft::constants::text::sdf_default_spread),
                 1, 1);
        scene.packet.draws.push_back(
            {{0, 0, weft::probe_constants::color_fixture_width, weft::probe_constants::color_fixture_height},
             {0, 0, 0, 1}});
        const auto glyph_draw = [&](const Cell& cell, float pen, float baseline, float scale) {
            const auto& b = cell.bitmap;
            Draw d{
                {pen + b.left * scale, baseline - b.top * scale, b.width * scale, b.height * scale}, {1, 1, 1, 1}, 0};
            d.uv = {cell.x / static_cast<float>(weft::probe_constants::sdf_atlas_width),
                    cell.y / static_cast<float>(weft::probe_constants::sdf_atlas_height),
                    b.width / static_cast<float>(weft::probe_constants::sdf_atlas_width),
                    b.height / static_cast<float>(weft::probe_constants::sdf_atlas_height)};
            return d;
        };
        // Same generated glyph, unchanged logical shape; only device quad scale varies.
        for (const auto& body : scene.squares) {
            const float scale = body.w / weft::constants::text::sdf_default_base_ppem;
            scene.packet.draws.push_back(glyph_draw(square, body.x, body.y + 32 * scale, scale));
        }
        auto stroke = glyph_draw(square, scene.stroked.x, scene.stroked.y + 16, 0.5f);
        stroke.stroke = 2;
        stroke.outline = {0, 0, 1, 1};
        scene.packet.draws.push_back(stroke);
        weft::Font roboto(WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf");
        const auto latin = roboto.shape("SDF", weft::constants::text::sdf_default_base_ppem);
        require(!latin.missing_glyph && latin.glyphs.size() == 3, "actual Latin glyph shape");
        UINT cursor = 70;
        std::vector<Cell> letters;
        for (const auto& glyph : latin.glyphs) {
            auto cell = pack(roboto.rasterize_sdf(glyph.glyph_id, weft::constants::text::sdf_default_base_ppem,
                                                  weft::constants::text::sdf_default_spread),
                             cursor, 1);
            cursor += cell.bitmap.width + 2;
            letters.push_back(std::move(cell));
        }
        for (const auto scale : {0.6f, 1.2f}) {
            float pen = scale < 1 ? 12.0f : 88.0f;
            const float baseline = scale < 1 ? 112.0f : 117.0f;
            for (std::size_t i = 0; i < letters.size(); ++i) {
                const auto& glyph = latin.glyphs[i];
                scene.packet.draws.push_back(
                    glyph_draw(letters[i], pen + glyph.offset_x * scale, baseline - glyph.offset_y * scale, scale));
                pen += glyph.advance_x * scale;
            }
        }
        // Poison immediately beyond each one-texel guard. Any atlas UV bleed is visible.
        for (UINT y = 0; y < 65; ++y) {
            for (UINT x = 58; x < 64; ++x) {
                atlas->bytes[y * weft::probe_constants::sdf_atlas_width + x] = 255;
            }
        }
        for (UINT y = 70; y < 74; ++y) {
            std::fill_n(atlas->bytes.data() + y * weft::probe_constants::sdf_atlas_width,
                        weft::probe_constants::sdf_atlas_width, std::uint8_t{255});
        }
        scene.packet.assets.push_back(std::move(atlas));
        return scene;
    }

    // The D3D11 reference renderer at the color fixture size, with the fixture's serials and its
    // self-checking full-path execution (cancellation, failure rollback, host state, readback).
    class Experiment : public Renderer {
    public:
        std::size_t assertions{};
        Color transparent_sample{};

        explicit Experiment(bool warp, UINT w = weft::probe_constants::color_fixture_width,
                            UINT h = weft::probe_constants::color_fixture_height)
            : Renderer(warp, w, h) {
            pending_completion = weft::probe_constants::fixture_host_serial;
        }

        std::vector<Pixel> execute(bool rtt, FramePacket packet = scene()) {
            auto frame = prepare(std::move(packet));
            // Cancellation and failure must not make unexecuted uploads Ready.
            auto cancelled = encode(frame, rtt);
            cancelled.commands.Reset();
            lifetime(tracker.DiscardEncoded(cancelled.token), "Cancel before submit");
            bool failure{};
            try {
                (void)encode(frame, rtt, true);
            } catch (const std::runtime_error& e) {
                failure = std::string(e.what()) == "injected recording failure";
            }
            test(failure, "partial recording failure cleanup");
            for (const auto& a : assets) {
                ResourceInfo state;
                lifetime(tracker.InspectResource(a.id, state), "Inspect after cancellation");
                test(state.phase == ResourcePhase::Created && state.encoded_pins == 0, "cancelled upload is not Ready");
            }
            auto encoded = encode(frame, rtt);
            UseTokenInfo token;
            lifetime(tracker.InspectUseToken(encoded.token, token), "Inspect upload plan");
            test(token.upload_count == assets.size(), "independent retry includes every upload");
            lifetime(tracker.ReleasePreparedFrame(frame.id), "ReleasePreparedFrame");
            frame.snapshot = {};
            for (const auto& a : assets) {
                lifetime(tracker.ReleaseResource(a.id), "Release CPU asset lease");
            }
            collect();
            test(native_releases == 0, "encoded pins survive CPU/prepared release");
            const D3D11_VIEWPORT host_viewport{3, 4, 17, 19, 0, 1};
            const D3D11_RECT host_scissor{5, 6, 15, 16};
            immediate->RSSetViewports(1, &host_viewport);
            immediate->RSSetScissorRects(1, &host_scissor);
            immediate->ExecuteCommandList(encoded.commands.Get(), TRUE);
            immediate->End(ui_done.Get());
            // Issue the diagnostic host completion marker before any throwing
            // post-submit assertion; failure cleanup can then drain actual work.
            immediate->CopyResource(staging.Get(), output.texture.Get());
            immediate->CopyResource(alpha_staging.Get(), group.texture.Get());
            immediate->End(host_done.Get());
            submitted = true;
            lifetime(tracker.NotifySubmitted(encoded.token, weft::probe_constants::immediate_queue,
                                             weft::probe_constants::fixture_ui_serial),
                     "NotifySubmitted after ExecuteCommandList");
            encoded.commands.Reset();
            D3D11_VIEWPORT restored_viewport{};
            UINT viewports = 1;
            immediate->RSGetViewports(&viewports, &restored_viewport);
            D3D11_RECT restored_scissor{};
            UINT scissors = 1;
            immediate->RSGetScissorRects(&scissors, &restored_scissor);
            test(viewports == 1 && std::memcmp(&restored_viewport, &host_viewport, sizeof(host_viewport)) == 0,
                 "host viewport restored");
            test(scissors == 1 && std::memcmp(&restored_scissor, &host_scissor, sizeof(host_scissor)) == 0,
                 "host scissor restored");
            // The host-owned copies have serial 43 and no UI use token.
            test(tracker.Shutdown() == LifetimeResult::Busy, "shutdown must drain actual submission");
            collect();
            test(native_releases == 0, "submission retains native textures until acknowledgement");
            for (const auto& a : assets) {
                ResourceInfo state;
                lifetime(tracker.InspectResource(a.id, state), "Inspect submitted texture");
                test(state.cpu_leases == 0 && state.prepared_pins == 0 && state.submitted_pins == 1,
                     "only submitted pin remains");
            }
            complete();
            test(native_releases == assets.size(), "native registry releases after actual completion");
            test(tracker.Stats().completed_serial == weft::probe_constants::fixture_host_serial &&
                     tracker.Stats().last_submitted_serial == weft::probe_constants::fixture_ui_serial,
                 "shared queue progresses beyond UI serial");
            test(tracker.Shutdown() == LifetimeResult::Ok, "shutdown drained");
            std::vector<Pixel> pixels(static_cast<std::size_t>(width) * height);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check_hresult(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map completed readback");
            for (UINT y = 0; y < height; ++y) {
                std::memcpy(pixels.data() + static_cast<std::size_t>(y) * width,
                            static_cast<const std::uint8_t*>(mapped.pData) +
                                static_cast<std::size_t>(y) * mapped.RowPitch,
                            width * sizeof(Pixel));
            }
            immediate->Unmap(staging.Get(), 0);
            check_hresult(immediate->Map(alpha_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                          "Map transparent readback");
            std::memcpy(transparent_sample.data(), mapped.pData, sizeof(Color));
            immediate->Unmap(alpha_staging.Get(), 0);
            const Color expected_alpha{0, 0.5f, 0, 0.5f};
            for (std::size_t c = 0; c < 4; ++c) {
                test(std::abs(transparent_sample[c] - expected_alpha[c]) <
                         weft::probe_constants::linear_alpha_tolerance,
                     "transparent linear premultiplied RGBA");
            }
            immediate->ClearState();
            return pixels;
        }

        void test(bool condition, const char* message) {
            require(condition, message);
            ++assertions;
        }
    };

    inline std::span<const std::uint8_t> pixel_bytes(const std::vector<Pixel>& pixels) {
        return {reinterpret_cast<const std::uint8_t*>(pixels.data()), pixels.size() * sizeof(Pixel)};
    }

    inline void save_png(const std::filesystem::path& path, const std::vector<Pixel>& pixels,
                         UINT w = weft::probe_constants::color_fixture_width,
                         UINT h = weft::probe_constants::color_fixture_height) {
        weft::testing::write_png(path, w, h, pixel_bytes(pixels));
    }

    inline std::string rgba(Pixel p) {
        std::ostringstream s;
        s << '[' << static_cast<int>(p[0]) << ',' << static_cast<int>(p[1]) << ',' << static_cast<int>(p[2]) << ','
          << static_cast<int>(p[3]) << ']';
        return s.str();
    }

    inline std::string linear_rgba(Color c) {
        std::ostringstream s;
        s << std::setprecision(weft::probe_constants::report_significant_digits) << '[' << c[0] << ',' << c[1] << ','
          << c[2] << ',' << c[3] << ']';
        return s.str();
    }

    inline std::vector<Pixel> read_png(const std::filesystem::path& path) {
        const auto image = weft::testing::read_png(path);
        require(image.width == width && image.height == height, "Browser capture must be 256x128 at DPR 1");
        std::vector<Pixel> pixels(static_cast<std::size_t>(image.width) * image.height);
        std::memcpy(pixels.data(), image.rgba.data(), image.rgba.size());
        return pixels;
    }

    inline float overlap(float pixel, float start, float length) {
        return std::clamp(std::min(pixel + 1, start + length) - std::max(pixel, start), 0.0f, 1.0f);
    }

    // Evidence text files end with a newline.
    inline void write_text(const std::filesystem::path& path, const std::string& text) {
        weft::testing::write_file(path, text + '\n');
    }

    inline FramePacket prepare_page_glyphs(const weft::Page& page, weft::Font& font) {
        FramePacket packet;

        struct Cached {
            int asset{-1};
            weft::GlyphBitmap bitmap;
        };

        std::map<std::pair<std::uint32_t, float>, Cached> cache;
        std::size_t bytes{};
        for (const auto& paint : page.paints) {
            if (paint.kind == weft::PaintKind::Rectangle) {
                packet.draws.push_back({{paint.x, paint.y, paint.width, paint.height}, paint.srgb});
                continue;
            }
            float pen_x = paint.x;
            float pen_y = paint.y;
            for (const auto& glyph : paint.shaped.glyphs) {
                const auto key = std::pair{glyph.glyph_id, paint.font_size};
                if (!cache.contains(key)) {
                    auto bitmap = font.rasterize(glyph.glyph_id, paint.font_size);
                    Cached cached;
                    if (bitmap.width && bitmap.height) {
                        bytes += bitmap.coverage.size();
                        require(bytes <= weft::probe_constants::page_glyph_max_bytes &&
                                    cache.size() < weft::probe_constants::max_frame_assets,
                                "page glyph budget");
                        cached.asset = static_cast<int>(packet.assets.size());
                        packet.assets.push_back(std::make_shared<const Asset>(
                            Asset{bitmap.width, bitmap.height, Encoding::Coverage, bitmap.coverage}));
                    }
                    cached.bitmap = std::move(bitmap);
                    cache.emplace(key, std::move(cached));
                }
                const auto& cached = cache.at(key);
                const auto& b = cached.bitmap;
                if (cached.asset >= 0) {
                    packet.draws.push_back({{pen_x + glyph.offset_x + b.left, pen_y - glyph.offset_y - b.top,
                                             static_cast<float>(b.width), static_cast<float>(b.height)},
                                            paint.srgb,
                                            cached.asset});
                }
                pen_x += glyph.advance_x;
                pen_y -= glyph.advance_y;
            }
        }
        return packet;
    }

    inline std::string page_json(const weft::Page& page) {
        std::ostringstream s;
        s << std::setprecision(weft::probe_constants::report_significant_digits) << "{\"boxes\":[";
        bool comma{};
        for (const auto& box : page.boxes) {
            if (comma) {
                s << ',';
            }
            comma = true;
            s << "{\"id\":" << json_string(box.id) << ",\"x\":" << box.x << ",\"y\":" << box.y
              << ",\"width\":" << box.width << ",\"height\":" << box.height << '}';
        }
        s << "],\"text_runs\":[";
        comma = false;
        for (const auto& p : page.paints) {
            if (p.kind == weft::PaintKind::Text) {
                if (comma) {
                    s << ',';
                }
                comma = true;
                s << "{\"id\":" << json_string(p.id) << ",\"text\":" << json_string(p.text) << ",\"x\":" << p.x
                  << ",\"baseline\":" << p.y << ",\"glyphs\":" << p.shaped.glyphs.size() << '}';
            }
        }
        return s.str() + "]}";
    }

    inline void pump_react(weft::ScriptDom& script, unsigned time) {
        for (unsigned batch = 0; batch < weft::constants::script::max_task_pump_turns; ++batch) {
            (void)script.eval("__ceHost.pump(" + std::to_string(time) + ")", "<host-timer-checkpoint>");
            // This separate eval happens after the previous microtask checkpoint.
            if (script.eval("__ceHost.pending()") == "0") {
                return;
            }
        }
        throw std::runtime_error("WEFT_TASK_LIMIT: React probe did not become idle");
    }

} // namespace probes
