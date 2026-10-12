#include "probe_common.hpp"
#include "cheetah_probe.hpp"
#include "probe_constants.hpp"
#include "probe_modes.hpp"

namespace probes {
    int run_render_cache(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        Experiment cached(warp, weft::probe_constants::small_target_size, weft::probe_constants::small_target_size);
        Experiment reference(warp, weft::probe_constants::small_target_size, weft::probe_constants::small_target_size);
        cached.cache_assets = true;
        cached.cache.resource_limit = weft::probe_constants::stress_cache_max_resources;
        // Traffic past the former 64 MiB threshold must preserve live IDs.
        auto stable = std::make_shared<const Asset>(Asset{1, 1, Encoding::Coverage, {255}});
        const auto stable_id = cached.asset_identity(stable);
        for (unsigned i = 0; i < weft::probe_constants::identity_traffic_bitmaps; ++i) {
            auto transient = std::make_shared<const Asset>(
                Asset{1024, 1024, Encoding::SrgbStraight, std::vector<std::uint8_t>(4 * 1024 * 1024, 255)});
            cached.asset_identity(transient);
            require(cached.asset_identity(stable) == stable_id,
                    "live texture identity changed under animated bitmap traffic");
        }
        std::uint64_t previous{};
        for (unsigned i = 0; i < weft::probe_constants::identity_churn_assets; ++i) {
            auto transient = std::make_shared<const Asset>(Asset{1, 1, Encoding::Coverage, {255}});
            auto id = cached.asset_identity(transient);
            require(id > previous && cached.asset_identity(transient) == id,
                    "recycled asset address aliased dead version");
            previous = id;
        }
        require(cached.asset_identity(stable) == stable_id &&
                    cached.asset_identities.size() <= weft::probe_constants::max_asset_identities,
                "identity metadata lost bounded stable asset");
        weft::FontSet fonts({WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf",
                             WEFT_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf"});
        CheetahPacketCache cpu;
        std::map<std::string, std::shared_ptr<const Asset>> images;
        auto image = [&](unsigned state) {
            auto a = std::make_shared<Asset>(Asset{4, 4, Encoding::SrgbStraight, std::vector<std::uint8_t>(64)});
            for (unsigned i = 0; i < 16; ++i) {
                a->bytes[i * 4] = std::uint8_t(state * 19 + i * 7);
                a->bytes[i * 4 + 1] = std::uint8_t(230 - state * 5);
                a->bytes[i * 4 + 2] = 80;
                a->bytes[i * 4 + 3] = i % 3 ? 255 : 0;
            }
            images["test"] = std::move(a);
        };
        image(0);
        auto page = [&](unsigned state) {
            weft::PositionedPage out;
            weft::PositionedPaint item;
            item.paint.kind = weft::PaintKind::Rectangle;
            item.paint.width = item.paint.height = weft::probe_constants::small_target_size;
            item.paint.srgb = {.1f, .1f, .1f, 1};
            item.clip = {0, 0, weft::probe_constants::small_target_size, weft::probe_constants::small_target_size};
            out.paints.push_back(item);
            const float x = 4 + float(state % 5);
            const float y = 4 + float(state % 7);
            const float r = 3 + float(state % 4);
            auto clip = weft::normalize_rounded({{x, y, 50, 46}, {r, r, r, r}, {r + 2, r + 2, r + 2, r + 2}});
            item.paint.x = x;
            item.paint.y = y;
            item.paint.width = 50;
            item.paint.height = 46;
            item.paint.srgb = {.2f, .6f, .4f, .6f};
            item.rounded_clips = {clip};
            out.paints.push_back(item);
            item.image_source = "test";
            item.paint.x = x - 2;
            item.paint.y = y - 2;
            item.paint.width = 55;
            item.paint.height = 48;
            item.uv = {.1f, .05f, .8f, .85f};
            out.paints.push_back(item);
            item.image_source.clear();
            item.paint.kind = weft::PaintKind::Text;
            item.paint.x = 8;
            item.paint.y = 32;
            item.paint.font_size = 12 + float(state % 3);
            item.paint.srgb = {.9f, .8f, .2f, 1};
            item.paint.shaped = fonts.shape("A仓", item.paint.font_size).shaped;
            out.paints.push_back(item);
            if (state % 2) {
                std::swap(out.paints[1], out.paints[2]);
            }
            return out;
        };
        auto read = [&](Experiment& renderer, FramePacket packet) {
            render_window(renderer, std::move(packet), [&] {
                renderer.immediate->CopyResource(renderer.staging.Get(), renderer.output.texture.Get());
            });
            std::vector<Pixel> pixels(weft::probe_constants::small_target_pixels);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check_hresult(renderer.immediate->Map(renderer.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                          "cache completed readback");
            for (UINT y = 0; y < weft::probe_constants::small_target_size; ++y) {
                std::memcpy(pixels.data() + y * weft::probe_constants::small_target_size,
                            static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                            weft::probe_constants::small_target_size * sizeof(Pixel));
            }
            renderer.immediate->Unmap(renderer.staging.Get(), 0);
            return pixels;
        };
        // Failure records no executable upload. Retrying a cached Created version
        // must still upload its bytes; a cache entry alone cannot mean Ready.
        auto first = cheetah_packet(page(0), fonts, images, &cpu);
        auto& backend = cached.ensure_backend();
        backend.inject_recording_failure = true;
        bool failed{};
        try {
            render_window(cached, first, [] {
            });
        } catch (const std::runtime_error&) {
            failed = true;
        }
        backend.inject_recording_failure = false;
        require(failed && !cached.tracker.Stats().encoded_tokens && !cached.tracker.Stats().submitted_tokens,
                "cached partial recording failure pins");
        for (const auto& [key, entry] : cached.cache.entries) {
            ResourceInfo info;
            lifetime(cached.tracker.InspectResource(entry.native.id, info), "cache failed upload state");
            require(info.phase == ResourcePhase::Created, "failed cached upload became Ready");
        }
        const auto retry = read(cached, first);
        const auto fresh = read(reference, cheetah_packet(page(0), fonts, images));
        int max_error{};
        auto compare = [&](const auto& a, const auto& b) {
            for (std::size_t i = 0; i < a.size(); ++i) {
                for (unsigned c = 0; c < 4; ++c) {
                    max_error = std::max(max_error, std::abs(int(a[i][c]) - int(b[i][c])));
                }
            }
            require(max_error <= weft::probe_constants::pipeline_pixel_tolerance, "cached/full mutation pixels differ");
        };
        compare(retry, fresh);
        // Deterministic mutations change clips/ellipses, fontsize, font IDs,
        // position, source versions and transparent paint order. Skip unpublished
        // packets as well: the next input must remain complete by itself.
        std::uint32_t seed = weft::probe_constants::render_cache_seed;
        unsigned frames{};
        for (unsigned i = 0; i < weft::probe_constants::render_cache_mutation_frames; ++i) {
            seed = seed * weft::constants::lcg_multiplier + weft::constants::lcg_increment;
            const auto state = seed % weft::probe_constants::mutation_states;
            if (i % 9 == 0) {
                image(i + 1);
            }
            if (i % 11 == 0) {
                auto dropped = cheetah_packet(page(state + 1), fonts, images, &cpu);
                require(!dropped.list->items.empty(), "dropped packet prepared");
            }
            compare(read(cached, cheetah_packet(page(state), fonts, images, &cpu)),
                    read(reference, cheetah_packet(page(state), fonts, images)));
            ++frames;
        }
        require(cached.cache.hits > 0 && cached.native_releases > 0 &&
                    cached.cache.entries.size() <= weft::probe_constants::stress_cache_max_resources,
                "cache reuse/eviction pressure absent");
        // A packet with more simultaneous dependencies than this deliberately
        // tiny cache must fail without an orphan tracked lease. Drain and reuse.
        FramePacket over_capacity;
        for (unsigned i = 0; i < weft::probe_constants::stress_cache_max_resources + 1; ++i) {
            over_capacity.assets.push_back(
                std::make_shared<const Asset>(Asset{1, 1, Encoding::Coverage, {std::uint8_t(i + 1)}}));
        }
        PacketView over_view(over_capacity, cached.window_serial + 1, &cached);
        weft_render_handle rejected{};
        require(weft_renderer_prepare(backend.renderer, &over_view.frame, &rejected) == WEFT_RENDER_BACKEND_ERROR &&
                    !rejected.slot && !cached.tracker.Stats().prepared_frames,
                "cache capacity rejection retained prepared frame");
        cached.evict_assets(0, true);
        require(!cached.tracker.Stats().resources && cached.native_creations == cached.native_releases,
                "cache capacity rejection orphaned texture lease");
        // Actual submitted work survives a host exception. Cache teardown must
        // reject until its event query proves completion.
        failed = false;
        try {
            render_window(cached, cheetah_packet(page(1), fonts, images, &cpu), [] {
                throw std::runtime_error("cache-post-submit");
            });
        } catch (const std::runtime_error& e) {
            failed = std::string(e.what()) == "cache-post-submit";
        }
        require(failed && cached.submitted && !cached.acknowledged, "cache post-submit failure lost submission");
        bool early{};
        try {
            cached.evict_assets(0, true);
        } catch (const std::runtime_error&) {
            early = true;
        }
        require(early, "cache released before actual completion");
        cached.complete();
        cached.evict_assets(0, true);
        reference.evict_assets(0, true);
        require(cached.native_creations == cached.native_releases && !cached.tracker.Stats().resources,
                "cached texture leak after query drain");
        lifetime(cached.tracker.Shutdown(), "cached shutdown");
        lifetime(reference.tracker.Shutdown(), "reference shutdown");
        cached.check_debug_messages();
        reference.check_debug_messages();
        std::ostringstream report;
        report
            << "{\"fixture\":\"render-cache-v1\",\"driver\":" << json_string(warp ? "warp" : "hardware")
            << ",\"mutation_frames\":" << frames << ",\"seed\":\"0xCE902026\",\"compared_pixels\":"
            << (frames + 1) * weft::probe_constants::small_target_pixels << ",\"max_difference\":" << max_error
            << ",\"native_creations\":" << cached.native_creations << ",\"native_releases\":" << cached.native_releases
            << ",\"cache_hits\":" << cached.cache.hits
            << ",\"capacity_failure_verified\":true,\"partial_retry_verified\":true,\"post_submit_drain_verified\":true,\"debug_warning_error_count\":0,\"passed\":true}";
        write_text(folder / "report.json", report.str());
        std::cout << "PASS cached/full paint mutation, source/font/clip/order, eviction and failure drain; max "
                  << max_error << '\n';
        return 0;
    }
} // namespace probes
