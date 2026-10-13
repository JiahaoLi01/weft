#pragma once

#include "effects/crpg_smoke.hpp"
#include "probe_common.hpp"
#include "scene/scene_effects.hpp"
#include "../tests/support/png.hpp"

#include <filesystem>
#include <memory>
#include <stdexcept>

// The UI shader effects the gallery's pages name with -weft-effect(): crpg-smoke, the CRPG UI's smoke background, with
// the captured cloud and noise bitmaps as its textures (their red channel, as the original reads them).
namespace probes {
    inline std::shared_ptr<const weft::paint::Asset> gallery_red_channel(const std::filesystem::path& path) {
        const auto image = weft::testing::read_png(path);
        std::vector<std::uint8_t> red(std::size_t(image.width) * image.height);
        for (std::size_t i = 0; i < red.size(); ++i) {
            red[i] = image.rgba[i * 4];
        }
        return std::make_shared<const weft::paint::Asset>(
            weft::paint::Asset{image.width, image.height, weft::paint::Encoding::Coverage, std::move(red)});
    }

    // The registry, built once per process; scenes of every thread read it.
    inline const weft::SceneEffects& gallery_effects() {
        static const auto effects = [] {
            const auto assets = std::filesystem::path(WEFT_SOURCE_DIR) / "tests/fixtures/crpg-gallery-assets/UI/public";
            auto registry = std::make_unique<weft::SceneEffects>();
            const auto added = registry->add(
                "crpg-smoke", weft::effects::crpg_smoke(),
                {gallery_red_channel(assets / "cloud.png"), gallery_red_channel(assets / "tiling-noise.png")});
            if (!added) {
                throw std::runtime_error(added.error().message);
            }
            return registry;
        }();
        return *effects;
    }
} // namespace probes
