// Real captured TSX -> native DOM -> positioned CSS values -> backend C ABI.
#include "probe_constants.hpp"

std::map<std::string, std::shared_ptr<const Asset>> load_cheetah_images() {
    std::map<std::string, std::shared_ptr<const Asset>> result;
    for (const auto* name : {"ink-panel-v2.png", "ink-dock-left-v1.png", "ink-dock-right-v1.png"}) {
        // Explicit host resource allowlist, no DOM-driven filesystem access.
        const auto path = std::filesystem::path(WEFT_SOURCE_DIR) / "tests/fixtures/casecheetah/UI/public/art" / name;
        auto image = weft::testing::read_png(path);
        require(image.width == weft::probe_constants::cheetah_art_width &&
                    image.height == weft::probe_constants::cheetah_art_height,
                "fixed source art dimensions");
        result.emplace(
            std::string("art/") + name,
            std::make_shared<Asset>(Asset{image.width, image.height, Encoding::SrgbStraight, std::move(image.rgba)}));
    }
    return result;
}

void crop_draw(Draw& draw, const std::array<float, 4>& clip) {
    const auto old = draw.rect;
    const float x = std::max(old.x, clip[0]);
    const float y = std::max(old.y, clip[1]);
    // Large device-space origins can make (x+w)-x slightly larger than w.
    // An intersected quad cannot exceed its source extent or UV interval.
    const float w = std::min(old.w, std::max(0.f, std::min(old.x + old.w, clip[0] + clip[2]) - x));
    const float h = std::min(old.h, std::max(0.f, std::min(old.y + old.h, clip[1] + clip[3]) - y));
    if (old.w <= 0 || old.h <= 0) {
        draw.rect.w = draw.rect.h = 0;
        return;
    }
    draw.uv = {draw.uv[0] + draw.uv[2] * (x - old.x) / old.w, draw.uv[1] + draw.uv[3] * (y - old.y) / old.h,
               draw.uv[2] * w / old.w, draw.uv[3] * h / old.h};
    draw.rect = {x, y, w, h};
}

struct CheetahPacketCache {
    using GlyphKey = std::tuple<std::uint32_t, std::uint32_t, float>;

    struct Glyph {
        weft::GlyphBitmap bitmap;
        std::shared_ptr<const Asset> asset;
    };

    struct MaskKey {
        std::shared_ptr<const Asset> input;
        std::vector<float> coordinates;

        bool operator<(const MaskKey& other) const {
            const std::less<const Asset*> less;
            if (less(input.get(), other.input.get())) {
                return true;
            }
            if (less(other.input.get(), input.get())) {
                return false;
            }
            return coordinates < other.coordinates;
        }
    };

    const void* font_scope{};
    std::map<GlyphKey, Glyph> glyphs;
    std::map<MaskKey, std::shared_ptr<const Asset>> masks;
    std::size_t glyph_bytes{};
    std::size_t mask_bytes{};
    std::size_t glyph_hits{};
    std::size_t mask_hits{};
    std::size_t glyph_misses{};
    std::size_t mask_misses{};

    void bind(const void* font) {
        if (font_scope && font_scope != font) {
            glyphs.clear();
            masks.clear();
            glyph_bytes = mask_bytes = 0;
        }
        font_scope = font;
    }
};

template <class FontT>
FramePacket cheetah_packet(const weft::PositionedPage& page, FontT& font,
                           const std::map<std::string, std::shared_ptr<const Asset>>& images,
                           CheetahPacketCache* persistent = nullptr) {
    FramePacket packet;
    std::map<std::string, int> image_indices;
    CheetahPacketCache local;
    auto& cache = persistent ? *persistent : local;
    cache.bind(&font);
    std::map<const Asset*, int> asset_indices;
    auto add_asset = [&](const std::shared_ptr<const Asset>& asset) {
        const auto [i, inserted] = asset_indices.emplace(asset.get(), int(packet.assets.size()));
        if (inserted) {
            packet.assets.push_back(asset);
        }
        return i->second;
    };
    std::size_t mask_bytes{};
    auto append = [&](Draw d, const std::array<float, 4>& clip, const std::vector<weft::RoundedClip>& masks) {
        crop_draw(d, clip);
        if (d.rect.w <= 0 || d.rect.h <= 0) {
            return;
        }
        bool full = true;
        for (const auto& c : masks) {
            if (!weft::contains_rounded(c, d.rect.x, d.rect.y) ||
                !weft::contains_rounded(c, d.rect.x + d.rect.w, d.rect.y) ||
                !weft::contains_rounded(c, d.rect.x, d.rect.y + d.rect.h) ||
                !weft::contains_rounded(c, d.rect.x + d.rect.w, d.rect.y + d.rect.h)) {
                full = false;
                break;
            }
        }
        if (!full) {
            require(d.rect.w <= weft::constants::render::rounded_mask_max_dimension &&
                        d.rect.h <= weft::constants::render::rounded_mask_max_dimension,
                    "rounded quad size bound");
            const auto w = UINT(std::ceil(d.rect.w));
            const auto h = UINT(std::ceil(d.rect.h));
            CheetahPacketCache::MaskKey key;
            if (persistent) {
                if (d.asset >= 0) {
                    key.input = packet.assets[d.asset];
                }
                key.coordinates = {d.rect.x, d.rect.y, d.rect.w, d.rect.h};
                key.coordinates.insert(key.coordinates.end(), d.uv.begin(), d.uv.end());
                for (const auto& c : masks) {
                    key.coordinates.insert(key.coordinates.end(), c.rect.begin(), c.rect.end());
                    key.coordinates.insert(key.coordinates.end(), c.radius_x.begin(), c.radius_x.end());
                    key.coordinates.insert(key.coordinates.end(), c.radius_y.begin(), c.radius_y.end());
                }
                if (const auto i = cache.masks.find(key); i != cache.masks.end()) {
                    ++cache.mask_hits;
                    d.asset = add_asset(i->second);
                    d.uv = {0, 0, 1, 1};
                    packet.draws.push_back(d);
                    return;
                }
                ++cache.mask_misses;
            }
            auto coverage = weft::rounded_coverage(masks, {d.rect.x, d.rect.y, d.rect.w, d.rect.h}, w, h);
            const Asset* old = d.asset < 0 ? nullptr : packet.assets[d.asset].get();
            const bool image = old && old->encoding == Encoding::SrgbStraight;
            require(!old || old->encoding != Encoding::SignedDistance, "rounded SDF preparation pending");
            const auto bytes = std::size_t(w) * h * (image ? weft::constants::rgba8_bytes_per_pixel : 1);
            require(bytes <= weft::probe_constants::packet_mask_max_bytes - mask_bytes,
                    "rounded preparation byte bound");
            mask_bytes += bytes;
            auto combined = std::make_shared<Asset>(
                Asset{w, h, image ? Encoding::SrgbStraight : Encoding::Coverage, std::vector<std::uint8_t>(bytes)});
            for (UINT y = 0; y < h; ++y) {
                for (UINT x = 0; x < w; ++x) {
                    auto index = std::size_t(y) * w + x;
                    float alpha = coverage[index] / weft::probe_constants::color_channel_max_float;
                    if (!old) {
                        combined->bytes[index] = coverage[index];
                        continue;
                    }
                    const float tx = (d.uv[0] + (x + .5f) / w * d.uv[2]) * old->width - .5f;
                    const float ty = (d.uv[1] + (y + .5f) / h * d.uv[3]) * old->height - .5f;
                    const int ix = int(std::floor(tx));
                    const int iy = int(std::floor(ty));
                    const float fx = tx - ix;
                    const float fy = ty - iy;
                    Color sampled{};
                    for (int yy = 0; yy < 2; ++yy) {
                        for (int xx = 0; xx < 2; ++xx) {
                            const auto px = std::clamp(ix + xx, 0, int(old->width) - 1);
                            const auto py = std::clamp(iy + yy, 0, int(old->height) - 1);
                            const auto j = std::size_t(py) * old->width + px;
                            float weight = (xx ? fx : 1 - fx) * (yy ? fy : 1 - fy);
                            if (!image) {
                                sampled[3] += old->bytes[j] / weft::probe_constants::color_channel_max_float * weight;
                            } else {
                                float a = old->bytes[j * 4 + 3] / weft::probe_constants::color_channel_max_float;
                                for (int c = 0; c < 3; ++c) {
                                    sampled[c] +=
                                        decode(old->bytes[j * 4 + c] / weft::probe_constants::color_channel_max_float) *
                                        a * weight;
                                }
                                sampled[3] += a * weight;
                            }
                        }
                    }
                    if (!image) {
                        combined->bytes[index] =
                            std::uint8_t(std::lround(alpha * sampled[3] * weft::constants::color_channel_max));
                    } else {
                        for (int c = 0; c < 3; ++c) {
                            float linear = sampled[3] > 0 ? std::clamp(sampled[c] / sampled[3], 0.f, 1.f) : 0;
                            float srgb = linear <= weft::probe_constants::srgb_encode_threshold
                                             ? linear * weft::probe_constants::srgb_linear_slope
                                             : weft::probe_constants::srgb_scale *
                                                       std::pow(linear, 1 / weft::probe_constants::srgb_gamma) -
                                                   weft::probe_constants::srgb_offset;
                            combined->bytes[index * 4 + c] =
                                std::uint8_t(std::lround(srgb * weft::constants::color_channel_max));
                        }
                        combined->bytes[index * 4 + 3] =
                            std::uint8_t(std::lround(sampled[3] * alpha * weft::constants::color_channel_max));
                    }
                }
            }
            if (persistent && bytes <= weft::probe_constants::mask_cache_max_bytes) {
                if (cache.masks.size() >= weft::probe_constants::mask_cache_max_entries ||
                    bytes > weft::probe_constants::mask_cache_max_bytes - cache.mask_bytes) {
                    cache.masks.clear();
                    cache.mask_bytes = 0;
                }
                cache.mask_bytes += bytes;
                cache.masks.emplace(std::move(key), combined);
            }
            d.asset = add_asset(combined);
            d.uv = {0, 0, 1, 1};
        }
        packet.draws.push_back(d);
    };
    for (const auto& item : page.paints) {
        const auto& p = item.paint;
        if (!item.image_source.empty()) {
            require(images.contains(item.image_source), "WEFT_RESOURCE_PROFILE: image not in fixture host allowlist");
            if (!image_indices.contains(item.image_source)) {
                image_indices[item.image_source] = add_asset(images.at(item.image_source));
            }
            Draw d{{p.x, p.y, p.width, p.height}, {1, 1, 1, 1}, image_indices.at(item.image_source)};
            d.uv = item.uv;
            append(d, item.clip, item.rounded_clips);
        } else if (p.kind == weft::PaintKind::Rectangle) {
            append({{p.x, p.y, p.width, p.height}, p.srgb}, item.clip, item.rounded_clips);
        } else {
            float pen_x = p.x;
            float pen_y = p.y;
            for (const auto& glyph : p.shaped.glyphs) {
                const auto key = std::tuple{glyph.font_id, glyph.glyph_id, p.font_size};
                if (!cache.glyphs.contains(key)) {
                    ++cache.glyph_misses;
                    CheetahPacketCache::Glyph cached;
                    cached.bitmap = font.rasterize(glyph, p.font_size);
                    auto& b = cached.bitmap;
                    const auto bytes = b.coverage.size();
                    require(!persistent || bytes <= weft::probe_constants::glyph_cache_max_bytes,
                            "glyph preparation cache byte bound");
                    if (persistent && (cache.glyphs.size() >= weft::probe_constants::glyph_cache_max_entries ||
                                       bytes > weft::probe_constants::glyph_cache_max_bytes - cache.glyph_bytes)) {
                        cache.glyphs.clear();
                        cache.glyph_bytes = 0;
                    }
                    if (b.width && b.height) {
                        cache.glyph_bytes += bytes;
                        cached.asset = std::make_shared<const Asset>(
                            Asset{b.width, b.height, Encoding::Coverage, std::move(b.coverage)});
                    }
                    cache.glyphs.emplace(key, std::move(cached));
                } else {
                    ++cache.glyph_hits;
                }
                const auto& cached = cache.glyphs.at(key);
                const auto& b = cached.bitmap;
                if (cached.asset) {
                    append({{pen_x + glyph.offset_x + b.left, pen_y - glyph.offset_y - b.top, float(b.width),
                             float(b.height)},
                            p.srgb,
                            add_asset(cached.asset)},
                           item.clip, item.rounded_clips);
                }
                pen_x += glyph.advance_x;
                pen_y -= glyph.advance_y;
            }
        }
    }
    require(packet.assets.size() < weft::probe_constants::max_frame_assets, "bounded game image/glyph/mask resources");
    return packet;
}

// Independent CPU sample oracle for bilinear premultiplied-linear image data.
Pixel cheetah_sample(const FramePacket& packet, int x, int y) {
    Color destination{0, 0, 0, 0};
    for (const auto& d : packet.draws) {
        if (x + .5f < d.rect.x || y + .5f < d.rect.y || x + .5f >= d.rect.x + d.rect.w ||
            y + .5f >= d.rect.y + d.rect.h) {
            continue;
        }
        Color source{decode(d.srgb[0]) * d.srgb[3], decode(d.srgb[1]) * d.srgb[3], decode(d.srgb[2]) * d.srgb[3],
                     d.srgb[3]};
        if (d.asset >= 0) {
            const auto& a = *packet.assets[d.asset];
            const float tx = (d.uv[0] + (x + .5f - d.rect.x) / d.rect.w * d.uv[2]) * a.width - .5f;
            const float ty = (d.uv[1] + (y + .5f - d.rect.y) / d.rect.h * d.uv[3]) * a.height - .5f;
            const int ix = int(std::floor(tx));
            const int iy = int(std::floor(ty));
            const float fx = tx - ix;
            const float fy = ty - iy;
            Color sample{};
            for (int yy = 0; yy < 2; ++yy) {
                for (int xx = 0; xx < 2; ++xx) {
                    const auto px = std::clamp(ix + xx, 0, int(a.width) - 1);
                    const auto py = std::clamp(iy + yy, 0, int(a.height) - 1);
                    const auto index = std::size_t(py) * a.width + px;
                    const float weight = (xx ? fx : 1 - fx) * (yy ? fy : 1 - fy);
                    if (a.encoding == Encoding::Coverage) {
                        sample[3] += a.bytes[index] / weft::probe_constants::color_channel_max_float * weight;
                    } else {
                        const float alpha = a.bytes[index * 4 + 3] / weft::probe_constants::color_channel_max_float;
                        for (int c = 0; c < 3; ++c) {
                            sample[c] +=
                                decode(a.bytes[index * 4 + c] / weft::probe_constants::color_channel_max_float) *
                                alpha * weight;
                        }
                        sample[3] += alpha * weight;
                    }
                }
            }
            if (a.encoding == Encoding::Coverage) {
                for (auto& c : source) {
                    c *= sample[3];
                }
            } else {
                for (int c = 0; c < 4; ++c) {
                    source[c] = sample[c];
                }
            }
        }
        for (auto& component : source) {
            component *= d.opacity;
        }
        for (int c = 0; c < 4; ++c) {
            destination[c] = source[c] + destination[c] * (1 - source[3]);
        }
    }
    Pixel result{};
    for (int c = 0; c < 4; ++c) {
        const float v = std::clamp(destination[c], 0.f, 1.f);
        const float e = c == 3 ? v
                        : v <= weft::probe_constants::srgb_encode_threshold
                            ? v * weft::probe_constants::srgb_linear_slope
                            : weft::probe_constants::srgb_scale * std::pow(v, 1 / weft::probe_constants::srgb_gamma) -
                                  weft::probe_constants::srgb_offset;
        result[c] = std::uint8_t(std::lround(e * weft::constants::color_channel_max));
    }
    return result;
}

int run_cheetah(bool warp, bool visible, const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    NativeReactWindow window(visible);
    SetWindowTextW(window.state.hwnd, L"CaseElephant - CaseCheetah original NineSlice / D3D11");
    Experiment gpu(warp, weft::constants::default_view_width, weft::constants::default_view_height);
    WindowPresenter presenter(gpu, window.state.hwnd);
    presenter.resize(weft::constants::default_view_width, weft::constants::default_view_height);
    presenter.capture = true;
    const auto images = load_cheetah_images();
    weft::Font font(WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf");
    weft::ScriptConfig config{weft::constants::script::probe_vm_stack_bytes};
#ifdef WEFT_REACT_GC_STRESS
    config.execution_budget_ms = weft::constants::script::max_execution_budget_ms;
#endif
    weft::ScriptDom script(config);
    weft::install_script_platform(script, weft::ScriptPlatformModule::WebHost);
    script.eval(read_file(WEFT_SOURCE_DIR "/examples/cheetah-nine-slice/native/app.js"));
    pump_react(script, 0);
    require(script.eval("caseCheetahFixture.version") == "19.3.0", "pinned fixture React runtime");
    const auto css = read_file(WEFT_SOURCE_DIR "/examples/cheetah-nine-slice/src/profile.css");
    const auto card = script.node_identity("#card");
    const auto image = script.node_identity(".nine-slice-art");
    std::array<weft::PositionedPage, weft::probe_constants::interaction_frames> pages;
    std::array<std::string, weft::probe_constants::interaction_frames> states;
    for (int i = 0; i < weft::probe_constants::interaction_frames; ++i) {
        if (i) {
            weft::MouseEvent e{weft::MouseType::Click, 250, 368};
            require(script.dispatch_mouse(script.node_identity("#resize"), e) != weft::DispatchResult::StaleTarget,
                    "host weak-identity native MouseEvent");
            pump_react(script, i * weft::probe_constants::fixture_frame_ms);
        }
        pages[i] =
            weft::build_positioned_page(script.snapshot_subtree("#gallery"), css, weft::constants::default_view_width,
                                        weft::constants::default_view_height, font);
        states[i] = script.eval("JSON.stringify(caseCheetahFixture.snapshot())");
        require(script.node_identity("#card") == card && script.node_identity(".nine-slice-art") == image,
                "original component wrappers retain native identities");
    }
    require(script.eval("caseCheetahFixture.errors.length") == "0" && script.stats().listener_exceptions == 0,
            "game component React error");
    write_text(folder / "native-dom.json", script.dump_document());
    if (!visible) {
        script.eval("caseCheetahFixture.unmount()");
        pump_react(script, 64);
        script.reset_document();
        script.collect_garbage();
    }
    std::ostringstream report;
    report << "{\"fixture\":\"casecheetah-nine-slice-v1\",\"driver\":" << json_string(warp ? "warp" : "hardware")
           << ",\"adapter\":" << json_string(gpu.adapter) << ",\"frames\":[";
    int max_error{};
    std::size_t samples{};
    std::vector<Pixel> initial;
    unsigned expected_releases{};
    auto render = [&](const auto& page) {
        auto packet = cheetah_packet(page, font, images);
        expected_releases += unsigned(packet.assets.size());
        gpu.render_window(packet, [&] {
            presenter.present();
        });
        return std::pair{std::move(packet), presenter.pixels()};
    };
    for (int i = 0; i < weft::probe_constants::interaction_frames; ++i) {
        auto [packet, pixels] = render(pages[i]);
        unsigned image_draws{};
        for (const auto& d : packet.draws) {
            if (d.asset >= 0 && packet.assets[d.asset]->encoding == Encoding::SrgbStraight) {
                ++image_draws;
            }
        }
        require(image_draws == 13, "original 9 card + 2 left + 2 right pieces reach GPU");
        for (int y = 3; y < weft::constants::default_view_height; y += 11) {
            for (int x = 2; x < weft::constants::default_view_width; x += 11) {
                const auto expected = cheetah_sample(packet, x, y);
                const auto actual = pixels[std::size_t(y) * weft::constants::default_view_width + x];
                for (int c = 0; c < 4; ++c) {
                    max_error = std::max(max_error, std::abs(int(actual[c]) - int(expected[c])));
                }
                ++samples;
            }
        }
        if (i == 0) {
            initial = pixels;
        } else if (i == 1) {
            require(pixels != initial, "React resize changes real GPU pixels");
        } else {
            require(pixels == initial, "React second resize restores full-image original exactly");
        }
        save_png(folder / ("frame-" + std::to_string(i) + ".png"), pixels, weft::constants::default_view_width,
                 weft::constants::default_view_height);
        if (i) {
            report << ',';
        }
        report << "{\"state\":" << states[i] << ",\"layout\":" << page_json(pages[i].geometry)
               << ",\"image_draws\":" << image_draws << ",\"textures\":" << packet.assets.size() << '}';
    }
    require(max_error <= weft::probe_constants::cpu_oracle_tolerance, "game PNG/crop/linear-alpha CPU oracle");
    gpu.check_debug_messages();
    require(expected_releases == gpu.native_releases, "all image/glyph resources retire after actual completion");
    report
        << "],\"original_component_verified\":true,\"original_css_rule_count\":5,\"original_png_count\":3,\"actual_completion_query\":true,\"native_textures_retired\":"
        << gpu.native_releases << ",\"cpu_pixel_samples\":" << samples << ",\"cpu_pixel_max_difference\":" << max_error
        << ",\"debug_layer\":" << (gpu.debug_layer ? "true" : "false")
        << ",\"debug_warning_error_count\":" << (gpu.debug_layer ? "0" : "null")
        << ",\"render_backend_abi_version\":1,\"mouse_geometry_hit_verified\":false,\"full_game_verified\":false,\"chinese_font_verified\":false,\"browser_pixel_parity_verified\":false,\"mobile_verified\":false,\"passed\":true}";
    write_text(folder / "report.json", report.str());
    std::cout << "PASS CaseCheetah original NineSlice: 3 PNGs, 13 cropped pieces, 3 frames; CPU pixel max " << max_error
              << "; " << gpu.native_releases << " textures retired\n";
    if (visible) {
        // Own window input: this isolated scaffold has one fixed known button.
        // General positioned-context hit testing is intentionally not claimed.
        const auto started = std::chrono::steady_clock::now();
        std::string previous = script.dump_document();
        bool pressed{};
        while (!window.state.closed) {
            window.messages();
            if (window.state.closed) {
                break;
            }
            const auto time = unsigned(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - started)
                                           .count()) +
                              100;
            script.eval("__ceHost.pump(" + std::to_string(time) + ")");
            while (!window.state.input.empty()) {
                auto v = window.state.input.front();
                window.state.input.pop_front();
                const auto p = v.viewport.logical(v.x, v.y);
                const bool inside = p[0] >= 228 && p[0] < 392 && p[1] >= 354 && p[1] < 386;
                if (v.action == weft::MouseAction::Down) {
                    pressed = inside;
                } else if (v.action == weft::MouseAction::Cancel) {
                    pressed = false;
                } else if (v.action == weft::MouseAction::Up) {
                    if (pressed && inside) {
                        script.dispatch_mouse(script.node_identity("#resize"), {weft::MouseType::Click, p[0], p[1]});
                        pump_react(script, time);
                    }
                    pressed = false;
                }
            }
            const auto dom = script.dump_document();
            if (!window.state.minimized && (dom != previous || window.state.repaint)) {
                if (window.state.resize) {
                    presenter.resize(window.state.width, window.state.height);
                    window.state.resize = false;
                }
                auto page = weft::build_positioned_page(script.snapshot_subtree("#gallery"), css,
                                                        weft::constants::default_view_width,
                                                        weft::constants::default_view_height, font);
                render(page);
                previous = dom;
                window.state.repaint = false;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(weft::probe_constants::visible_poll_ms)));
        }
    }
    return 0;
}
