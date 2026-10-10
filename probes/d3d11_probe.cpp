#include "layout/auto_page.hpp"
#include "script/script_platform.hpp"
#include "layout/flow_page.hpp"
#include "scene/scene.hpp"
#include "scene/scene_native.hpp"
#include "core/owner_executor.hpp"
#define NOMINMAX
#include "render/resource_lifetime.hpp"
#include "text/font.hpp"
#include "layout/page.hpp"
#include "layout/positioned_page.hpp"
#include "input/mouse_router.hpp"
#include "weft/render_backend_d3d11.h"
#include "render/backends/d3d11/host.hpp"
#include "render/backends/d3d11/two_frame_pipeline.hpp"
#include "render/srgb.hpp"
// Constants and test-support headers must be seen here, at global scope: probe headers included
// inside the anonymous namespace below would otherwise declare (anonymous)::caseelephant.
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

// An independent M0 host/renderer experiment. No DOM or stable renderer API.
namespace {
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

    void lifetime(LifetimeResult r, const char* operation) {
        require(r == LifetimeResult::Ok, operation);
    }

    float decode(float c) {
        return weft::render::srgb_to_linear(c);
    }

    using Pixel = std::array<std::uint8_t, 4>;
    using Color = std::array<float, 4>;
    using Rect = weft::geometry::Rect;

    FramePacket scene() {
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

    SdfScene sdf_scene() {
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

    struct Check {
        const char* id;
        UINT x;
        UINT y;
        Pixel expected;
    };

    const std::array<Check, 10> checks{{{"white-half", 32, 32, {188, 188, 188, 255}},
                                        {"ordered-red-blue", 96, 32, {137, 0, 188, 255}},
                                        {"srgb-gray-input", 160, 32, {128, 128, 128, 255}},
                                        {"coverage-128", 224, 32, {188, 188, 188, 255}},
                                        {"parent-opacity-group", 32, 96, {0, 0, 188, 255}},
                                        {"premultiplied-image-edge", 96, 96, {188, 0, 0, 255}},
                                        {"actual-freetype-glyph", 160, 96, {255, 255, 255, 255}},
                                        {"glyph-exterior", 136, 72, {0, 0, 0, 255}},
                                        {"scissor-interior", 224, 96, {255, 0, 0, 255}},
                                        {"scissor-exterior", 200, 72, {0, 0, 0, 255}}}};

    std::span<const std::uint8_t> pixel_bytes(const std::vector<Pixel>& pixels) {
        return {reinterpret_cast<const std::uint8_t*>(pixels.data()), pixels.size() * sizeof(Pixel)};
    }

    void save_png(const std::filesystem::path& path, const std::vector<Pixel>& pixels,
                  UINT w = weft::probe_constants::color_fixture_width,
                  UINT h = weft::probe_constants::color_fixture_height) {
        weft::testing::write_png(path, w, h, pixel_bytes(pixels));
    }

    std::string rgba(Pixel p) {
        std::ostringstream s;
        s << '[' << static_cast<int>(p[0]) << ',' << static_cast<int>(p[1]) << ',' << static_cast<int>(p[2]) << ','
          << static_cast<int>(p[3]) << ']';
        return s.str();
    }

    std::string linear_rgba(Color c) {
        std::ostringstream s;
        s << std::setprecision(weft::probe_constants::report_significant_digits) << '[' << c[0] << ',' << c[1] << ','
          << c[2] << ',' << c[3] << ']';
        return s.str();
    }

    std::vector<Pixel> read_png(const std::filesystem::path& path) {
        const auto image = weft::testing::read_png(path);
        require(image.width == width && image.height == height, "Browser capture must be 256x128 at DPR 1");
        std::vector<Pixel> pixels(static_cast<std::size_t>(image.width) * image.height);
        std::memcpy(pixels.data(), image.rgba.data(), image.rgba.size());
        return pixels;
    }

    int capture_browser(const std::filesystem::path& png, const std::filesystem::path& output) {
        const auto pixels = read_png(png);
        std::ostringstream report;
        report
            << "{\"fixture\":\"caseelephant-d3d11-color-v1\",\"width\":256,\"height\":128,\"source\":\"actual Chrome PNG capture\",\"samples\":[";
        bool comma{};
        for (const auto& check : checks) {
            if (comma) {
                report << ',';
            }
            comma = true;
            report << "{\"id\":" << json_string(check.id)
                   << ",\"rgba\":" << rgba(pixels[static_cast<std::size_t>(check.y) * width + check.x]) << '}';
        }
        report << "]}\n";
        std::filesystem::create_directories(output.parent_path());
        std::ofstream file(output, std::ios::binary);
        require(bool(file << report.str()), "write browser color samples");
        std::cout << "Captured " << checks.size() << " actual browser PNG samples\n";
        return 0;
    }

    int run(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        Experiment direct(warp);
        Experiment rtt(warp);
        auto direct_pixels = direct.execute(false);
        auto rtt_pixels = rtt.execute(true);
        std::ostringstream report;
        report
            << "{\"fixture\":\"caseelephant-d3d11-color-v1\",\"driver\":" << json_string(warp ? "warp" : "hardware")
            << ",\"adapter\":" << json_string(direct.adapter)
            << ",\"feature_level\":\"11_0\",\"debug_layer\":" << (direct.debug_layer ? "true" : "false")
            << ",\"width\":256,\"height\":128,\"color_contract\":\"linear-srgb-premultiplied\",\"tolerance_rgba8\":2,\"checks\":[";
        bool comma{};
        for (const auto& check : checks) {
            const auto p = direct_pixels[static_cast<std::size_t>(check.y) * width + check.x];
            const auto q = rtt_pixels[static_cast<std::size_t>(check.y) * width + check.x];
            for (std::size_t c = 0; c < 4; ++c) {
                direct.test(std::abs(int(p[c]) - int(check.expected[c])) <=
                                (c == 3 ? 0 : weft::probe_constants::rgba8_parity_tolerance),
                            check.id);
                rtt.test(std::abs(int(q[c]) - int(check.expected[c])) <=
                             (c == 3 ? 0 : weft::probe_constants::rgba8_parity_tolerance),
                         check.id);
            }
            if (comma) {
                report << ',';
            }
            comma = true;
            report << "{\"id\":" << json_string(check.id) << ",\"expected\":" << rgba(check.expected)
                   << ",\"direct\":" << rgba(p) << ",\"rtt\":" << rgba(q) << '}';
        }
        int max_difference{};
        for (std::size_t i = 0; i < direct_pixels.size(); ++i) {
            for (std::size_t c = 0; c < 4; ++c) {
                max_difference = std::max(max_difference, std::abs(int(direct_pixels[i][c]) - int(rtt_pixels[i][c])));
            }
        }
        direct.test(max_difference <= weft::probe_constants::rgba8_parity_tolerance, "full-image direct/RTT parity");
        direct.check_debug_messages();
        rtt.check_debug_messages();
        report
            << "],\"direct_rtt_max_difference\":" << max_difference
            << ",\"assertions\":" << direct.assertions + rtt.assertions
            << ",\"native_textures_retired\":" << direct.native_releases + rtt.native_releases
            << ",\"transparent_linear_direct\":" << linear_rgba(direct.transparent_sample)
            << ",\"transparent_linear_rtt\":" << linear_rgba(rtt.transparent_sample)
            << ",\"debug_warning_error_count\":" << (direct.debug_layer ? "0" : "null")
            << ",\"actual_completion_query\":true,\"ui_serial\":42,\"host_completed_serial\":43,\"dom_pipeline_verified\":false,\"mobile_verified\":false,\"passed\":true}\n";
        save_png(folder / "direct.png", direct_pixels);
        save_png(folder / "rtt.png", rtt_pixels);
        std::ofstream file(folder / "report.json", std::ios::binary);
        require(bool(file << report.str()), "write GPU report");
        std::cout << "PASS D3D11 " << (warp ? "WARP" : "hardware") << ": " << direct.adapter << ", "
                  << direct.assertions + rtt.assertions
                  << " assertions; real query completion; direct/RTT max difference " << max_difference
                  << "; debug layer " << (direct.debug_layer ? "on" : "unavailable") << '\n';
        return 0;
    }

    float overlap(float pixel, float start, float length) {
        return std::clamp(std::min(pixel + 1, start + length) - std::max(pixel, start), 0.0f, 1.0f);
    }

    int run_sdf(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        const auto scene = sdf_scene();
        Experiment direct(warp);
        Experiment rtt(warp);
        const auto p = direct.execute(false, scene.packet);
        const auto q = rtt.execute(true, scene.packet);
        float max_edge_error{};
        std::size_t edge_samples{};
        std::size_t partial_samples{};
        std::size_t latin_pixels{};
        int max_difference{};
        for (std::size_t i = 0; i < p.size(); ++i) {
            direct.test(p[i][3] == weft::constants::color_channel_max && q[i][3] == weft::constants::color_channel_max,
                        "SDF opaque background alpha");
            for (std::size_t c = 0; c < 4; ++c) {
                max_difference = std::max(max_difference, std::abs(int(p[i][c]) - int(q[i][c])));
            }
        }
        // Independent area-coverage oracle for axis-aligned straight square edges.
        // Exclude two-edge corner neighborhoods: a single-channel field rounds corners.
        for (const auto& r : scene.squares) {
            float case_error{};
            std::size_t partial{};
            for (int y = static_cast<int>(r.y) - weft::probe_constants::sdf_edge_scan_margin;
                 y < r.y + r.h + weft::probe_constants::sdf_edge_scan_margin; ++y) {
                for (int x = static_cast<int>(r.x) - weft::probe_constants::sdf_edge_scan_margin;
                     x < r.x + r.w + weft::probe_constants::sdf_edge_scan_margin; ++x) {
                    const bool near_x = std::abs(x + 0.5f - r.x) < weft::probe_constants::sdf_corner_exclusion ||
                                        std::abs(x + 0.5f - r.x - r.w) < weft::probe_constants::sdf_corner_exclusion;
                    const bool near_y = std::abs(y + 0.5f - r.y) < weft::probe_constants::sdf_corner_exclusion ||
                                        std::abs(y + 0.5f - r.y - r.h) < weft::probe_constants::sdf_corner_exclusion;
                    if (near_x && near_y) {
                        continue;
                    }
                    const float expected =
                        overlap(static_cast<float>(x), r.x, r.w) * overlap(static_cast<float>(y), r.y, r.h);
                    const auto& pixel = p[static_cast<std::size_t>(y) * width + x];
                    direct.test(pixel[0] == pixel[1] && pixel[1] == pixel[2], "SDF square neutral fill");
                    const float error =
                        std::abs(decode(pixel[0] / weft::probe_constants::color_channel_max_float) - expected);
                    case_error = std::max(case_error, error);
                    ++edge_samples;
                    if (expected > 0 && expected < 1) {
                        ++partial;
                        if (pixel[0] > 0 && pixel[0] < weft::constants::color_channel_max) {
                            ++partial_samples;
                        }
                    }
                }
            }
            max_edge_error = std::max(max_edge_error, case_error);
            direct.test(partial > 40, "subpixel square edges sampled");
            if (case_error > weft::probe_constants::sdf_edge_tolerance) {
                std::cerr << "scale=" << r.w / weft::constants::text::sdf_default_base_ppem
                          << " edge error=" << case_error << '\n';
            }
            direct.test(case_error <= weft::probe_constants::sdf_edge_tolerance,
                        "SDF straight-edge area coverage (0.12 linear tolerance)");
        }
        direct.test(partial_samples > 200, "derivative AA produced partial coverage at all scales");
        float stroke_error{};
        for (int x = 208; x < 237; ++x) {
            const auto& pixel = p[24 * width + x];
            const float fill = overlap(static_cast<float>(x), scene.stroked.x, 20);
            const float outer = overlap(static_cast<float>(x), scene.stroked.x - 1, 22);
            stroke_error = std::max(
                {stroke_error, std::abs(decode(pixel[0] / weft::probe_constants::color_channel_max_float) - fill),
                 std::abs(decode(pixel[2] / weft::probe_constants::color_channel_max_float) - outer)});
            direct.test(pixel[0] == pixel[1], "blue stroke leaves equal red/green fill");
        }
        direct.test(stroke_error <= weft::probe_constants::sdf_edge_tolerance, "SDF stroke source-distance coverage");
        for (UINT y = 78; y < weft::probe_constants::color_fixture_height; ++y) {
            for (UINT x = 0; x < 210; ++x) {
                if (p[y * width + x][0] > 200) {
                    ++latin_pixels;
                }
            }
        }
        direct.test(latin_pixels > 600, "actual Roboto SDF visible at two sizes");
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                bool covered{};
                for (const auto& d : scene.packet.draws) {
                    if (d.asset >= 0 && x + 0.5f >= d.rect.x && y + 0.5f >= d.rect.y &&
                        x + 0.5f < d.rect.x + d.rect.w && y + 0.5f < d.rect.y + d.rect.h) {
                        covered = true;
                    }
                }
                if (!covered) {
                    direct.test(p[y * width + x] == Pixel{0, 0, 0, 255}, "no glyph pixels outside quads");
                }
            }
        }
        direct.test(max_difference <= weft::probe_constants::rgba8_parity_tolerance,
                    "SDF full-image direct/RTT parity");
        direct.check_debug_messages();
        rtt.check_debug_messages();
        save_png(folder / "direct.png", p);
        save_png(folder / "rtt.png", q);
        // Distance diagnostic: encoded byte image, not a coverage/glyph screenshot.
        std::vector<Pixel> distance_image;
        distance_image.reserve(width * height);
        for (const auto value : scene.packet.assets[0]->bytes) {
            distance_image.push_back({value, value, value, 255});
        }
        save_png(folder / "distance-atlas.png", distance_image);
        std::ostringstream report;
        report
            << std::setprecision(weft::probe_constants::report_significant_digits)
            << "{\"fixture\":\"caseelephant-sdf-v1\",\"driver\":" << json_string(warp ? "warp" : "hardware")
            << ",\"adapter\":" << json_string(direct.adapter)
            << ",\"debug_layer\":" << (direct.debug_layer ? "true" : "false")
            << ",\"width\":256,\"height\":128,\"generator\":\"FreeType-2.13.3-bsdf\",\"base_ppem\":40,\"spread_source_texels\":8"
            << ",\"zero_byte\":128,\"encoding\":\"R8_UNORM-linear-distance\",\"atlas_bytes\":32768,\"atlas_mip_levels\":1,\"guard_texels\":1"
            << ",\"generated_glyphs\":" << scene.generated_glyphs
            << ",\"glyph_distance_bytes\":" << scene.distance_bytes
            << ",\"square_scales\":[0.5,1,1.5],\"latin_ppem\":[24,48],\"stroke_source_texels\":2,\"edge_samples\":"
            << edge_samples << ",\"partial_coverage_samples\":" << partial_samples
            << ",\"straight_edge_max_linear_error\":" << max_edge_error
            << ",\"straight_edge_tolerance\":0.12,\"corner_oracle_verified\":false,\"stroke_max_linear_error\":"
            << stroke_error << ",\"latin_visible_pixels\":" << latin_pixels
            << ",\"direct_rtt_max_difference\":" << max_difference
            << ",\"assertions\":" << direct.assertions + rtt.assertions
            << ",\"native_textures_retired\":" << direct.native_releases + rtt.native_releases
            << ",\"actual_completion_query\":true,\"ui_serial\":42,\"host_completed_serial\":43,\"debug_warning_error_count\":"
            << (direct.debug_layer ? "0" : "null")
            << ",\"dom_pipeline_verified\":false,\"mobile_verified\":false,\"msdf_verified\":false,\"passed\":true}\n";
        std::ofstream file(folder / "report.json", std::ios::binary);
        require(bool(file << report.str()), "write SDF report");
        std::cout << "PASS SDF D3D11 " << (warp ? "WARP" : "hardware") << ": " << direct.adapter << "; "
                  << direct.assertions + rtt.assertions << " assertions; straight edge max error " << max_edge_error
                  << "; stroke " << stroke_error << "; direct/RTT " << max_difference << '\n';
        return 0;
    }

    // Evidence text files end with a newline.
    void write_text(const std::filesystem::path& path, const std::string& text) {
        weft::testing::write_file(path, text + '\n');
    }

    FramePacket prepare_page_glyphs(const weft::Page& page, weft::Font& font) {
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

    std::string page_json(const weft::Page& page) {
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

    void pump_react(weft::ScriptDom& script, unsigned time) {
        for (unsigned batch = 0; batch < weft::constants::script::max_task_pump_turns; ++batch) {
            (void)script.eval("__ceHost.pump(" + std::to_string(time) + ")", "<host-timer-checkpoint>");
            // This separate eval happens after the previous microtask checkpoint.
            if (script.eval("__ceHost.pending()") == "0") {
                return;
            }
        }
        throw std::runtime_error("WEFT_TASK_LIMIT: React probe did not become idle");
    }

    int run_react(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        weft::ScriptConfig config{weft::constants::script::probe_vm_stack_bytes};
#ifdef WEFT_REACT_GC_STRESS
        config.execution_budget_ms = weft::constants::script::max_execution_budget_ms;
#endif
        weft::ScriptDom script(config);
        weft::install_script_platform(script, weft::ScriptPlatformModule::WebHost);
        script.eval(read_file(std::filesystem::path(WEFT_SOURCE_DIR) / "examples/react-vite/native/app.js"),
                    "react-vite-app.js");
        pump_react(script, 0);
        require(script.eval("caseElephantReact.version") == "19.3.0", "fixed real React version");
        require(script.eval("caseElephantReact.errors.length") == "0", "React initial error");
        const auto item = script.node_identity("#item-ember");
        const auto button = script.node_identity("#increment");
        weft::Font font(WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf");
        std::array<weft::Page, weft::probe_constants::interaction_frames> pages;
        std::array<std::string, weft::probe_constants::interaction_frames> states;
        pages[0] = weft::build_page(script.snapshot_subtree("#page"), font);
        states[0] = script.eval("JSON.stringify(caseElephantReact.snapshot())");
        script.eval("document.getElementById('increment').dispatchEvent(new Event('click',{bubbles:true}));");
        pump_react(script, weft::probe_constants::fixture_frame_ms);
        require(script.eval("document.getElementById('count').textContent") == "Count: 1",
                "React useState click update");
        require(script.node_identity("#increment") == button, "React state update replaced button identity");
        pages[1] = weft::build_page(script.snapshot_subtree("#page"), font);
        states[1] = script.eval("JSON.stringify(caseElephantReact.snapshot())");
        script.eval("document.getElementById('reverse').dispatchEvent(new Event('click',{bubbles:true}));");
        pump_react(script, 2 * weft::probe_constants::fixture_frame_ms);
        require(script.eval("caseElephantReact.snapshot().order.join(',')") == "moss,tide,ember",
                "React keyed reorder");
        require(script.node_identity("#item-ember") == item, "keyed update did not retain native node identity");
        require(script.eval("caseElephantReact.errors.length") == "0" && script.stats().listener_exceptions == 0,
                "React interaction error");
        pages[2] = weft::build_page(script.snapshot_subtree("#page"), font);
        states[2] = script.eval("JSON.stringify(caseElephantReact.snapshot())");
        write_text(folder / "native-dom.json", script.dump_document());
        script.eval("caseElephantReact.unmount();");
        pump_react(script, 3 * weft::probe_constants::fixture_frame_ms);
        script.collect_garbage();
        require(script.eval("document.getElementById('root').childNodes.length") == "0", "React root unmount");
        script.reset_document(); // GPU and page value snapshots cannot depend on the VM.
        require(!script.is_live(item), "page epoch reset left old identity alive");
        std::ostringstream report;
        report << "{\"fixture\":\"caseelephant-react-vite-v1\",\"react\":\"19.3.0\",\"vite\":\"8.3.3\",\"driver\":"
               << json_string(warp ? "warp" : "hardware") << ",\"script_budget_ms\":" << config.execution_budget_ms
               << ",\"frames\":[";
        std::vector<Pixel> initial_pixels;
        std::vector<Pixel> count_pixels;
        std::size_t retired{};
        std::size_t assertions{};
        bool debug = true;
        for (std::size_t i = 0; i < pages.size(); ++i) {
            auto packet = prepare_page_glyphs(pages[i], font);
            const auto textures = packet.assets.size();
            const auto draws = packet.draws.size();
            Experiment direct(warp, weft::constants::default_view_width, weft::constants::default_view_height);
            Experiment rtt(warp, weft::constants::default_view_width, weft::constants::default_view_height);
            auto p = direct.execute(false, packet);
            auto q = rtt.execute(true, std::move(packet));
            int difference{};
            for (std::size_t pixel = 0; pixel < p.size(); ++pixel) {
                for (std::size_t c = 0; c < 4; ++c) {
                    difference = std::max(difference, std::abs(int(p[pixel][c]) - int(q[pixel][c])));
                }
            }
            direct.test(difference <= weft::probe_constants::rgba8_parity_tolerance,
                        "React full-image direct/RTT parity");
            direct.test(p[8 * weft::constants::default_view_width + 8] == Pixel{16, 21, 30, 255},
                        "React inline background native pixels");
            const Pixel first_color = i == 2 ? Pixel{146, 200, 121, 255} : Pixel{244, 162, 97, 255};
            direct.test(p[166 * weft::constants::default_view_width + 330] == first_color,
                        "React keyed bars reach GPU paint order");
            std::size_t text_pixels{};
            for (UINT y = 48; y < 84; ++y) {
                for (UINT x = 24; x < 220; ++x) {
                    if (p[y * weft::constants::default_view_width + x][0] > 120) {
                        ++text_pixels;
                    }
                }
            }
            direct.test(text_pixels > 400, "actual React title glyph pixels");
            if (i == 0) {
                initial_pixels = p;
            }
            if (i == 1) {
                std::size_t changed{};
                bool outside_unchanged = true;
                for (UINT y = 0; y < weft::constants::default_view_height; ++y) {
                    for (UINT x = 0; x < weft::constants::default_view_width; ++x) {
                        if (p[y * weft::constants::default_view_width + x] !=
                            initial_pixels[y * weft::constants::default_view_width + x]) {
                            if (x >= 40 && x < 208 && y >= 158 && y < 198) {
                                ++changed;
                            } else {
                                outside_unchanged = false;
                            }
                        }
                    }
                }
                direct.test(changed > 30 && outside_unchanged, "useState changes only counter text region");
                count_pixels = p;
            }
            if (i == 2) {
                std::size_t changed{};
                for (UINT y = 158; y < 258; ++y) {
                    for (UINT x = 256; x < 600; ++x) {
                        if (p[y * weft::constants::default_view_width + x] !=
                            count_pixels[y * weft::constants::default_view_width + x]) {
                            ++changed;
                        }
                    }
                }
                direct.test(changed > 500, "keyed reorder changes actual list pixels");
            }
            direct.check_debug_messages();
            rtt.check_debug_messages();
            debug = debug && direct.debug_layer && rtt.debug_layer;
            retired += direct.native_releases + rtt.native_releases;
            assertions += direct.assertions + rtt.assertions;
            save_png(folder / ("frame-" + std::to_string(i) + ".png"), p, weft::constants::default_view_width,
                     weft::constants::default_view_height);
            save_png(folder / ("frame-" + std::to_string(i) + "-rtt.png"), q, weft::constants::default_view_width,
                     weft::constants::default_view_height);
            if (i) {
                report << ',';
            }
            report << "{\"state\":" << states[i] << ",\"layout\":" << page_json(pages[i])
                   << ",\"textures\":" << textures << ",\"draws\":" << draws
                   << ",\"direct_rtt_max_difference\":" << difference << '}';
        }
        report
            << "],\"assertions\":" << assertions << ",\"native_textures_retired\":" << retired
            << ",\"debug_layer\":" << (debug ? "true" : "false")
            << ",\"debug_warning_error_count\":" << (debug ? "0" : "null")
            << ",\"actual_completion_query\":true,\"keyed_identity_preserved\":true,\"native_event_dispatch_verified\":true,\"snapshots_survive_vm_reset\":true,\"root_unmount_verified\":true"
            << ",\"html_parser_verified\":false,\"stylesheet_cascade_verified\":false,\"native_window_input_verified\":false,\"mobile_verified\":false,\"passed\":true}";
        write_text(folder / "report.json", report.str());
        std::cout << "PASS React/Vite native DOM -> inline CSS -> Yoga -> HarfBuzz/FreeType -> D3D11 "
                  << (warp ? "WARP" : "hardware") << "; 3 real pixel frames; " << retired << " textures retired; "
                  << assertions << " assertions\n";
        return 0;
    }

#include "react_window_host.hpp"
#include "render_backend_probe.hpp"
#include "cheetah_probe.hpp"
#include "cheetah_alert_probe.hpp"
#include "flow_probe.hpp"
#include "cheetah_flow_window_probe.hpp"
#include "render_cache_probe.hpp"
#include "two_frame_probe.hpp"
#include "gallery_canvas_probe.hpp"
#include "gallery_scene_session.hpp"
#include "scene_native_gpu_probe.hpp"
#include "gallery_layer_manifest.hpp"
#include "scene_gpu_surface_probe.hpp"
#include "scene_surface_retirement_probe.hpp"
#include "gallery_cluster_gpu_probe.hpp"
#include "gallery_interaction_profile.hpp"
#include "gallery_owner_probe.hpp"
#include "gallery_preview_probe.hpp"
#include "external_texture_probe.hpp"
#include "complex_css_probe.hpp"
#include "render_stages_probe.hpp"
} // namespace

int main(int argc, char** argv) {
    try {
        const bool owner_async = argc == 3 && (std::string(argv[1]) == "--owner-async-test-hardware" ||
                                               std::string(argv[1]) == "--owner-async-test-warp");
        const bool render_stages = argc == 3 && (std::string(argv[1]) == "--render-stages-test-hardware" ||
                                                 std::string(argv[1]) == "--render-stages-test-warp");
        const bool complex_css_test = argc == 3 && (std::string(argv[1]) == "--complex-css-test-hardware" ||
                                                    std::string(argv[1]) == "--complex-css-test-warp");
        const bool scene_native_test = argc == 3 && (std::string(argv[1]) == "--scene-native-test-hardware" ||
                                                     std::string(argv[1]) == "--scene-native-test-warp");
        const bool scene_gpu_test = argc == 3 && (std::string(argv[1]) == "--scene-gpu-test-hardware" ||
                                                  std::string(argv[1]) == "--scene-gpu-test-warp");
        const bool gallery_profile = argc == 4 && std::string(argv[1]) == "--gallery-owner-profile";
        const bool gallery_input_benchmark = argc == 4 && std::string(argv[1]) == "--gallery-input-benchmark-hardware";
        const bool gallery_benchmark = argc == 4 && std::string(argv[1]) == "--gallery-benchmark-hardware";
        const bool surface_retirement = argc == 3 && (std::string(argv[1]) == "--scene-surface-retirement-hardware" ||
                                                      std::string(argv[1]) == "--scene-surface-retirement-warp");
        const bool gallery_scroll = argc == 3 && (std::string(argv[1]) == "--gallery-scroll-test-hardware" ||
                                                  std::string(argv[1]) == "--gallery-scroll-test-warp");
        const bool gallery_cluster = argc == 3 && (std::string(argv[1]) == "--gallery-cluster-gpu-hardware" ||
                                                   std::string(argv[1]) == "--gallery-cluster-gpu-warp");
        const bool gallery_trace = argc == 3 && (std::string(argv[1]) == "--gallery-window-profile-hardware" ||
                                                 std::string(argv[1]) == "--gallery-window-profile-warp");
        const bool gallery_preview = argc == 3 && (std::string(argv[1]) == "--gallery-window-hardware" ||
                                                   std::string(argv[1]) == "--gallery-window-warp" ||
                                                   std::string(argv[1]) == "--gallery-window-test-hardware" ||
                                                   std::string(argv[1]) == "--gallery-window-test-warp");
        const bool external_texture = argc == 3 && (std::string(argv[1]) == "--external-texture-warp" ||
                                                    std::string(argv[1]) == "--external-texture-hardware");
        const bool gallery_canvas_window =
            argc == 3 && (std::string(argv[1]) == "--gallery-canvas-window-hardware" ||
                          std::string(argv[1]) == "--gallery-canvas-window-warp" ||
                          std::string(argv[1]) == "--gallery-canvas-window-test-warp" ||
                          std::string(argv[1]) == "--gallery-canvas-window-test-hardware");
        const bool gallery_canvas = argc == 3 && (std::string(argv[1]) == "--gallery-canvas-warp" ||
                                                  std::string(argv[1]) == "--gallery-canvas-hardware");
        const bool pipeline_probe =
            argc == 3 && (std::string(argv[1]) == "--pipeline-warp" || std::string(argv[1]) == "--pipeline-hardware");
        const bool cache_probe =
            argc == 3 && (std::string(argv[1]) == "--cache-warp" || std::string(argv[1]) == "--cache-hardware");
        const bool browser = argc == 4 && std::string(argv[1]) == "--browser-png";
        const bool sdf =
            argc == 3 && (std::string(argv[1]) == "--sdf-warp" || std::string(argv[1]) == "--sdf-hardware");
        const bool react =
            argc == 3 && (std::string(argv[1]) == "--react-warp" || std::string(argv[1]) == "--react-hardware");
        const bool backend =
            argc == 3 && (std::string(argv[1]) == "--backend-warp" || std::string(argv[1]) == "--backend-hardware");
        const bool cheetah =
            argc == 3 && (std::string(argv[1]) == "--cheetah-warp" || std::string(argv[1]) == "--cheetah-hardware" ||
                          std::string(argv[1]) == "--cheetah-window-hardware");
        const bool automatic = argc == 3 && (std::string(argv[1]) == "--cheetah-auto-warp" ||
                                             std::string(argv[1]) == "--cheetah-auto-hardware");
        const bool cheetah_flow = argc == 3 && (std::string(argv[1]) == "--cheetah-flow-warp" ||
                                                std::string(argv[1]) == "--cheetah-flow-hardware");
        const bool flow_window =
            argc == 3 && (std::string(argv[1]) == "--cheetah-flow-window-test-4k-motion-warp" ||
                          std::string(argv[1]) == "--cheetah-flow-window-test-4k-motion-hardware" ||
                          std::string(argv[1]) == "--cheetah-flow-window-test-warp" ||
                          std::string(argv[1]) == "--cheetah-flow-window-test-hardware" ||
                          std::string(argv[1]) == "--cheetah-flow-window-warp" ||
                          std::string(argv[1]) == "--cheetah-flow-window-hardware" ||
                          std::string(argv[1]) == "--cheetah-flow-window-test-4k-warp" ||
                          std::string(argv[1]) == "--cheetah-flow-window-test-4k-hardware" ||
                          std::string(argv[1]) == "--cheetah-flow-window-perf-hardware" ||
                          std::string(argv[1]) == "--cheetah-flow-window-perf-sync-hardware");
        const bool flow =
            argc == 3 && (std::string(argv[1]) == "--flow-warp" || std::string(argv[1]) == "--flow-hardware");
        const bool theme = argc == 3 && (std::string(argv[1]) == "--cheetah-style-warp" ||
                                         std::string(argv[1]) == "--cheetah-style-hardware");
        const bool alert = argc == 3 && (std::string(argv[1]) == "--cheetah-alert-warp" ||
                                         std::string(argv[1]) == "--cheetah-alert-hardware");
        const bool window = argc == 3 && (std::string(argv[1]) == "--react-window-hardware" ||
                                          std::string(argv[1]) == "--react-window-warp" ||
                                          std::string(argv[1]) == "--react-window-test-hardware" ||
                                          std::string(argv[1]) == "--react-window-test-warp");
        require(
            gallery_input_benchmark || gallery_trace || gallery_profile || surface_retirement || gallery_scroll ||
                gallery_cluster || owner_async || scene_native_test || render_stages || complex_css_test ||
                scene_gpu_test || gallery_benchmark || gallery_preview || external_texture || gallery_canvas_window ||
                gallery_canvas || pipeline_probe || cache_probe || flow_window || cheetah_flow || flow || automatic ||
                browser || sdf || react || window || backend || cheetah || alert || theme ||
                (argc == 3 && (std::string(argv[1]) == "--warp" || std::string(argv[1]) == "--hardware")),
            "Usage: weft_d3d11_probe --gallery-window-[test-]hardware|warp <output-directory> (or an existing probe mode)");
        check_hresult(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
        try {
            const auto result =
                gallery_input_benchmark ? run_gallery_preview(false, argv[2], false, std::string("input-") + argv[3])
                : gallery_trace
                    ? run_gallery_preview(std::string(argv[1]).ends_with("warp"), argv[2], false, {}, true, false, true)
                : gallery_profile    ? run_gallery_interaction_profile(argv[2], argv[3])
                : surface_retirement ? run_scene_surface_retirement(std::string(argv[1]).ends_with("warp"), argv[2])
                : gallery_scroll
                    ? run_gallery_preview(std::string(argv[1]).ends_with("warp"), argv[2], true, {}, true, true)
                : gallery_cluster   ? run_gallery_cluster_gpu(std::string(argv[1]).ends_with("warp"), argv[2])
                : owner_async       ? run_owner_async(std::string(argv[1]).ends_with("warp"), argv[2])
                : scene_native_test ? run_scene_gpu_test(std::string(argv[1]).ends_with("warp"), argv[2], true)
                : render_stages     ? run_render_stages(std::string(argv[1]).ends_with("warp"), argv[2])
                : complex_css_test  ? run_complex_css_test(std::string(argv[1]).ends_with("warp"), argv[2])
                : scene_gpu_test    ? run_scene_gpu_test(std::string(argv[1]).ends_with("warp"), argv[2])
                : gallery_benchmark ? run_gallery_preview(false, argv[2], false, argv[3])
                : gallery_preview   ? run_gallery_preview(std::string(argv[1]).ends_with("warp"), argv[2],
                                                          std::string(argv[1]).find("-test-") != std::string::npos)
                : external_texture  ? run_external_texture(std::string(argv[1]).ends_with("warp"), argv[2])
                : gallery_canvas_window
                    ? run_gallery_canvas_window(std::string(argv[1]).ends_with("warp"), argv[2],
                                                std::string(argv[1]).find("-test-") != std::string::npos)
                : gallery_canvas ? run_gallery_canvas(std::string(argv[1]).ends_with("warp"), argv[2])
                : pipeline_probe ? run_two_frame_probe(std::string(argv[1]).ends_with("warp"), argv[2])
                : cache_probe    ? run_render_cache(std::string(argv[1]).ends_with("warp"), argv[2])
                : flow_window    ? run_cheetah_flow_window(std::string(argv[1]).ends_with("warp"),
                                                           std::string(argv[1]).find("-test-") != std::string::npos ||
                                                               std::string(argv[1]).find("-perf-") != std::string::npos,
                                                           argv[2],
                                                           std::string(argv[1]).find("-test-") == std::string::npos ||
                                                               std::string(argv[1]).find("-4k-") != std::string::npos,
                                                           std::string(argv[1]).find("-perf-") != std::string::npos,
                                                           std::string(argv[1]).find("-sync-") != std::string::npos,
                                                           std::string(argv[1]).find("-motion-") != std::string::npos)
                : cheetah_flow
                    ? run_cheetah_alert(std::string(argv[1]) == "--cheetah-flow-warp", argv[2], true, true, true)
                : flow      ? run_flow(std::string(argv[1]) == "--flow-warp", argv[2])
                : automatic ? run_cheetah_alert(std::string(argv[1]) == "--cheetah-auto-warp", argv[2], true, true)
                : theme     ? run_cheetah_alert(std::string(argv[1]) == "--cheetah-style-warp", argv[2], true)
                : alert     ? run_cheetah_alert(std::string(argv[1]) == "--cheetah-alert-warp", argv[2])
                : cheetah   ? run_cheetah(std::string(argv[1]) == "--cheetah-warp",
                                          std::string(argv[1]) == "--cheetah-window-hardware", argv[2])
                : backend   ? run_backend(std::string(argv[1]) == "--backend-warp", argv[2])
                : window    ? run_react_window(std::string(argv[1]).ends_with("warp"),
                                               std::string(argv[1]).find("-test-") != std::string::npos, argv[2])
                : browser   ? capture_browser(argv[2], argv[3])
                : react     ? run_react(std::string(argv[1]) == "--react-warp", argv[2])
                : sdf       ? run_sdf(std::string(argv[1]) == "--sdf-warp", argv[2])
                            : run(std::string(argv[1]) == "--warp", argv[2]);
            CoUninitialize();
            return result;
        } catch (...) {
            CoUninitialize();
            throw;
        }
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
