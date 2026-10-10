#pragma once
#include "scene/scene.hpp"
#include "support/png.hpp"
#include <filesystem>

// Evidence PNG of a native scene raster.
inline void png(const std::filesystem::path& path, const weft::SceneBitmap& image) {
    weft::testing::write_png(path, image.width, image.height, image.rgba);
}

#include "scene_visual_compare.hpp"
