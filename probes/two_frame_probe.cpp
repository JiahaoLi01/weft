#include "probe_common.hpp"
#include "probe_constants.hpp"
#include "probe_modes.hpp"

namespace probes {
    int run_two_frame_probe(bool warp, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        Experiment gpu(warp, weft::probe_constants::small_target_size, weft::probe_constants::small_target_size);
        Experiment reference(warp, weft::probe_constants::small_target_size, weft::probe_constants::small_target_size);
        gpu.cache_assets = true;
        gpu.cache_resource_limit = weft::probe_constants::pipeline_cache_max_resources;
        auto coverage = std::make_shared<const Asset>(
            Asset{4, 4, Encoding::Coverage, {0, 64, 128, 255, 255, 128, 64, 0, 0, 128, 255, 64, 255, 64, 128, 0}});
        auto packet = [&](unsigned state) {
            auto image = std::make_shared<Asset>(Asset{4, 4, Encoding::SrgbStraight, std::vector<std::uint8_t>(64)});
            for (unsigned i = 0; i < 16; ++i) {
                image->bytes[i * 4] = std::uint8_t(state * 19 + i * 7);
                image->bytes[i * 4 + 1] = std::uint8_t(230 - state * 5);
                image->bytes[i * 4 + 2] = 80;
                image->bytes[i * 4 + 3] = i % 3 ? 255 : 0;
            }
            FramePacket p;
            p.assets = {coverage, image};
            p.draws = {{{0, 0, weft::probe_constants::small_target_size, weft::probe_constants::small_target_size},
                        {.1f, .15f, .2f, 1}},
                       {{float(state % 13), float(state % 9), 45, 48}, {1, 1, 1, 1}, 1},
                       {{9, 12, 40, 35}, {.3f, .8f, .4f, .6f}, 0}};
            if (state % 2) {
                std::swap(p.draws[1], p.draws[2]);
            }
            return p;
        };
        std::array<ComPtr<ID3D11Texture2D>, weft::probe_constants::frames_in_flight> readbacks;
        for (auto& texture : readbacks) {
            D3D11_TEXTURE2D_DESC desc{};
            gpu.staging->GetDesc(&desc);
            check_hresult(gpu.device->CreateTexture2D(&desc, nullptr, &texture), "independent pipeline frame readback");
        }
        auto pixels = [&](Experiment& renderer, ID3D11Texture2D* texture) {
            std::vector<Pixel> out(weft::probe_constants::small_target_pixels);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check_hresult(renderer.immediate->Map(texture, 0, D3D11_MAP_READ, 0, &mapped), "completed pipeline pixels");
            for (UINT y = 0; y < weft::probe_constants::small_target_size; ++y) {
                std::memcpy(out.data() + y * weft::probe_constants::small_target_size,
                            static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                            weft::probe_constants::small_target_size * sizeof(Pixel));
            }
            renderer.immediate->Unmap(texture, 0);
            return out;
        };
        int max_error{};
        unsigned compared{};
        unsigned frames{};
        std::size_t peak{};
        std::size_t submitted{};
        std::size_t completed{};
        {
            TwoFramePipeline pipeline(gpu);
            auto& backend = gpu.ensure_backend();
            backend.inject_recording_failure = true;
            bool failed{};
            try {
                pipeline.submit(packet(1), [] {
                });
            } catch (const std::runtime_error&) {
                failed = true;
            }
            backend.inject_recording_failure = false;
            require(failed && !pipeline.in_flight() && !gpu.tracker.Stats().encoded_tokens,
                    "failed encoding consumed pipeline slot");
            // Same coverage submitted by frame 1 is consumed by frame 2 before
            // completion, without a redundant upload or dependency on cancellation.
            const auto shared_id = gpu.asset_identity(coverage);
            auto host_lease = std::make_shared<const Asset>(Asset{1, 1, Encoding::Coverage, {255}});
            std::weak_ptr<const void> old_lease = host_lease;
            pipeline.submit(
                packet(2),
                [] {
                },
                host_lease);
            host_lease.reset();
            const auto uploads = gpu.uploads;
            pipeline.submit(packet(3), [] {
            });
            require(pipeline.in_flight() == weft::probe_constants::frames_in_flight &&
                        gpu.tracker.Stats().submitted_tokens == weft::probe_constants::frames_in_flight &&
                        gpu.uploads == uploads + 1,
                    "same-queue uploaded asset not reusable in next frame");
            ResourceInfo info;
            lifetime(gpu.tracker.InspectResource(gpu.asset_cache.at({shared_id, 1}).native.id, info),
                     "two-frame shared resource pins");
            require(info.phase == ResourcePhase::Submitted &&
                        info.submitted_pins == weft::probe_constants::frames_in_flight && !old_lease.expired(),
                    "in-flight resource/host lease released early");
            bool early{};
            try {
                gpu.evict_assets(0, true);
            } catch (const std::runtime_error&) {
                early = true;
            }
            require(early, "pipeline cache drain accepted pending queries");
            pipeline.drain();
            require(old_lease.expired() && !gpu.tracker.Stats().submitted_tokens,
                    "actual completion failed to retire two slots");
            std::uint32_t seed = weft::probe_constants::two_frame_seed;
            for (unsigned round = 0; round < weft::probe_constants::two_frame_rounds; ++round) {
                std::array<FramePacket, weft::probe_constants::frames_in_flight> values;
                for (unsigned i = 0; i < weft::probe_constants::frames_in_flight; ++i) {
                    seed = seed * weft::constants::lcg_multiplier + weft::constants::lcg_increment;
                    values[i] = packet(seed % weft::probe_constants::mutation_states);
                    if (round % 7 == 0) {
                        auto dropped = packet(seed % weft::probe_constants::mutation_states + 1);
                        require(!dropped.draws.empty(), "complete dropped frame");
                    }
                    pipeline.submit(values[i], [&, i] {
                        gpu.immediate->CopyResource(readbacks[i].Get(), gpu.output.texture.Get());
                    });
                }
                require(pipeline.in_flight() == weft::probe_constants::frames_in_flight,
                        "test did not queue two independent submissions");
                pipeline.drain();
                for (unsigned i = 0; i < weft::probe_constants::frames_in_flight; ++i) {
                    auto actual = pixels(gpu, readbacks[i].Get());
                    render_window(reference, values[i], [&] {
                        reference.immediate->CopyResource(reference.staging.Get(), reference.output.texture.Get());
                    });
                    auto expected = pixels(reference, reference.staging.Get());
                    for (std::size_t p = 0; p < actual.size(); ++p) {
                        for (unsigned c = 0; c < 4; ++c) {
                            max_error = std::max(max_error, std::abs(int(actual[p][c]) - int(expected[p][c])));
                        }
                    }
                    require(max_error <= weft::probe_constants::pipeline_pixel_tolerance,
                            "two-frame/full path pixels differ or constant buffers overwritten");
                    compared += weft::probe_constants::small_target_pixels;
                    ++frames;
                }
            }
            // Two queued records survive a host exception after Execute; the next
            // submit observes bounded backpressure and completion, never a cancel.
            pipeline.submit(packet(4), [] {
            });
            failed = false;
            try {
                pipeline.submit(packet(5), [] {
                    throw std::runtime_error("pipeline-post-submit");
                });
            } catch (const std::runtime_error& e) {
                failed = std::string(e.what()) == "pipeline-post-submit";
            }
            require(failed && pipeline.in_flight() == weft::probe_constants::frames_in_flight &&
                        gpu.tracker.Stats().submitted_tokens == weft::probe_constants::frames_in_flight,
                    "post-submit failure lost actual in-flight work");
            pipeline.submit(packet(6), [] {
            });
            require(pipeline.in_flight() <= weft::probe_constants::frames_in_flight,
                    "third submission exceeded bounded queue");
            pipeline.drain();
            // Leave both slots pending for the destructor: close must drain.
            pipeline.submit(packet(7), [] {
            });
            pipeline.submit(packet(8), [] {
            });
            peak = pipeline.peak_in_flight;
            submitted = pipeline.submissions;
            completed = pipeline.completions;
        }
        require(peak == weft::probe_constants::frames_in_flight && !gpu.async_in_flight &&
                    !gpu.tracker.Stats().submitted_tokens && gpu.tracker.Stats().completed_serial == gpu.window_serial,
                "close did not drain true two-frame completion");
        completed += weft::probe_constants::frames_in_flight;
        require(submitted == completed, "pipeline submission/completion accounting");
        gpu.evict_assets(0, true);
        reference.evict_assets(0, true);
        require(gpu.native_creations == gpu.native_releases, "pipeline texture leak after drain");
        lifetime(gpu.tracker.Shutdown(), "pipeline shutdown");
        lifetime(reference.tracker.Shutdown(), "pipeline reference shutdown");
        gpu.check_debug_messages();
        reference.check_debug_messages();
        std::ostringstream report;
        report
            << "{\"fixture\":\"two-frame-v1\",\"driver\":" << json_string(warp ? "warp" : "hardware")
            << ",\"seed\":\"0xCE102026\",\"mutation_frames\":" << frames << ",\"compared_pixels\":" << compared
            << ",\"max_difference\":" << max_error << ",\"pipeline_depth\":2,\"peak_in_flight\":" << peak
            << ",\"submissions\":" << submitted << ",\"completions\":" << completed
            << ",\"completed_serial\":" << gpu.tracker.Stats().completed_serial
            << ",\"native_creations\":" << gpu.native_creations << ",\"native_releases\":" << gpu.native_releases
            << ",\"submitted_upload_reuse_verified\":true,\"post_submit_failure_verified\":true,\"close_drain_verified\":true,\"host_lease_retirement_verified\":true,\"actual_event_queries\":true,\"debug_warning_error_count\":0,\"passed\":true}";
        write_text(folder / "report.json", report.str());
        std::cout
            << "PASS two-frame actual query pipeline, upload reuse, independent pixels, failures and close drain; max "
            << max_error << '\n';
        return 0;
    }
} // namespace probes
