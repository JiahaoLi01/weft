// Images drawn smaller and larger than they are, by the native Metal pass, against Chrome: the page of
// tests/fixtures/image_scale (page.js builds img elements and element backgrounds of test images at fixed places, sizes
// and image-rendering values, cases.json) in a View of the reference's size, drawn by the Metal scene renderer and read
// back over white, against tools/browser_image_scale_reference.mjs's screenshot of the same page in Chrome (chrome.png),
// case by case over each case's rectangle. What it checks: the levels of a page's image (scene_image_mips), the level
// and filtering Chrome draws it at (trilinear, half a level sharp, one tile; level 0 for a pattern of tiles), filtering
// in encoded sRGB, and pixelated images' nearest texels (scene.hlsl's fittedTexture). A case passes when each of its
// pixels differs from Chrome's by at most `level` in every channel, but a pixelated img's pixels whose centers fall
// exactly between two texels: Chrome takes either texel there, by the rounding of its raster pipeline's arithmetic (one
// way in some columns, the other in the next), which the test counts and does not hold the GPU to. A side-by-side image
// (weft on Metal left, Chrome right) goes to the output folder. Objective-C++ (ARC); PNGs through ImageIO.
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>

#import "render/backends/metal/scene_renderer.hpp"

#include "support/check.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "view/view.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
    using weft::testing::require;
    namespace fs = std::filesystem;

    // Channel levels a pixel may differ by: the GPU's filter weights and level fractions are coarser than Skia's.
    constexpr int level = 4;

    struct Rgba {
        unsigned width{};
        unsigned height{};
        std::vector<std::uint8_t> rgba;
    };

    // A PNG's pixels as sRGB RGBA8 rows (the reference is opaque).
    Rgba read_png(const fs::path& path) {
        NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
        CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
        require(source != nullptr, "cannot read " + path.string());
        CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
        CFRelease(source);
        require(image != nullptr, "cannot decode " + path.string());
        Rgba out{unsigned(CGImageGetWidth(image)), unsigned(CGImageGetHeight(image)), {}};
        out.rgba.resize(std::size_t(out.width) * out.height * 4);
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context =
            CGBitmapContextCreate(out.rgba.data(), out.width, out.height, 8, out.width * 4, space,
                                  CGBitmapInfo(kCGImageAlphaPremultipliedLast) | CGBitmapInfo(kCGBitmapByteOrder32Big));
        CGContextDrawImage(context, CGRectMake(0, 0, out.width, out.height), image);
        CGContextRelease(context);
        CGColorSpaceRelease(space);
        CGImageRelease(image);
        return out;
    }

    void write_png(const fs::path& path, unsigned width, unsigned height, std::vector<std::uint8_t> rgba) {
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context =
            CGBitmapContextCreate(rgba.data(), width, height, 8, width * 4, space,
                                  CGBitmapInfo(kCGImageAlphaPremultipliedLast) | CGBitmapInfo(kCGBitmapByteOrder32Big));
        CGImageRef image = CGBitmapContextCreateImage(context);
        NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
        CGImageDestinationRef out =
            CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
        require(out != nullptr, "cannot write " + path.string());
        CGImageDestinationAddImage(out, image, nullptr);
        CGImageDestinationFinalize(out);
        CFRelease(out);
        CGImageRelease(image);
        CGContextRelease(context);
        CGColorSpaceRelease(space);
    }

    std::optional<std::vector<std::uint8_t>> read_resource(std::string_view path) {
        std::ifstream in(std::string(path), std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
    }
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            const auto out = fs::path(argc > 1 ? argv[1] : "artifacts/image-scale-metal");
            fs::create_directories(out);
            const auto directory = weft::testing::source_path("tests/fixtures/image_scale");
            const auto manifest = weft::testing::parse_json(weft::testing::read_file(directory / "cases.json"));
            const auto width = unsigned(manifest["width"].number);
            const auto height = unsigned(manifest["height"].number);
            const auto chrome = read_png(directory / "chrome.png");
            require(chrome.width == width && chrome.height == height, "the reference's size");

            const auto fonts = std::make_shared<weft::FontCatalog>();
            weft::SceneText text{*fonts};
            weft::View view{fonts, text, float(width), float(height), 1};
            view.set_resource_reader(read_resource);
            weft::ViewPage page;
            page.base = directory.generic_string() + "/";
            page.scripts.push_back({"page.js", weft::testing::read_text_file(directory / "page.js")});
            // Chrome's page: its body at 0, 0 over white.
            page.stylesheet = "#root{position:relative;width:" + std::to_string(width) +
                              "px;height:" + std::to_string(height) + "px;background:#ffffff}";
            page.forgiving_css = true;
            page.web_platform = true;
            view.load(page);
            for (int k = 1; k <= 3; ++k) {
                view.advance(k * 16.0);
            }
            const auto diagnostics = view.take_diagnostics();
            require(diagnostics.empty(), diagnostics.empty() ? std::string() : diagnostics.front().message);

            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            require(device != nil, "a Metal device");
            weft::render::metal::SceneRenderer renderer(device, text);
            const auto ours = renderer.draw(view.scene(), 1, width, height, std::nullopt, 0, {});
            require(ours.has_value(), "native Metal pass: " + renderer.last_failure());
            // Encoded premultiplied BGRA over white, as RGBA.
            std::vector<std::uint8_t> weft(std::size_t(width) * height * 4);
            for (std::size_t i = 0; i < std::size_t(width) * height; ++i) {
                const int a = (*ours)[i * 4 + 3];
                for (int c = 0; c < 3; ++c) {
                    weft[i * 4 + c] = std::uint8_t(std::min(255, int((*ours)[i * 4 + 2 - c]) + 255 - a));
                }
                weft[i * 4 + 3] = 255;
            }

            int failed{};
            int compared{};
            for (const auto& c : manifest["cases"].array) {
                const auto& r = c["rect"].array;
                const auto x0 = unsigned(r[0].number);
                const auto y0 = unsigned(r[1].number);
                const auto w = unsigned(r[2].number);
                const auto h = unsigned(r[3].number);
                // A pixelated img's ties: a pixel whose center, (i + 1/2) natural / size texels in, is a whole number
                // of texels along either axis.
                const auto style = c["style"].string + ";" + (c.find("parent") ? c["parent"].string : std::string());
                const bool pixelated = c["tag"].string == "img" && (style.find("image-rendering:pixelated") !=
                                                                        std::string::npos ||
                                                                    style.find("image-rendering:crisp-edges") !=
                                                                        std::string::npos);
                std::array<unsigned, 2> natural{};
                if (pixelated) {
                    const auto image = read_png(directory / c["src"].string);
                    natural = {image.width, image.height};
                }
                const auto tie = [&](unsigned i, unsigned length, unsigned texels) {
                    return pixelated && (2 * i + 1) * texels % (2 * length) == 0;
                };
                std::size_t over{};
                std::size_t ties{};
                double mean{};
                int max{};
                for (unsigned y = y0; y < y0 + h; ++y) {
                    for (unsigned x = x0; x < x0 + w; ++x) {
                        const auto i = (std::size_t(y) * width + x) * 4;
                        int worst{};
                        for (int k = 0; k < 3; ++k) {
                            worst = std::max(worst, std::abs(int(weft[i + k]) - int(chrome.rgba[i + k])));
                        }
                        if (worst > level && (tie(x - x0, w, natural[0]) || tie(y - y0, h, natural[1]))) {
                            ++ties;
                            continue;
                        }
                        over += worst > level;
                        mean += worst;
                        max = std::max(max, worst);
                    }
                }
                const double pixels = double(w) * h;
                const bool passed = over == 0;
                ++compared;
                failed += !passed;
                std::cout << (passed ? "ok   " : "FAIL ") << std::left << std::setw(32) << c["name"].string
                          << std::right << std::fixed << std::setprecision(2) << " mean " << mean / pixels << ", max "
                          << std::setw(3) << max << ", " << over << " over " << level;
                if (ties) {
                    std::cout << " (" << ties << " ties take the other texel)";
                }
                std::cout << '\n';
            }

            std::vector<std::uint8_t> pair(std::size_t(width) * 2 * height * 4);
            for (unsigned y = 0; y < height; ++y) {
                std::copy_n(&weft[std::size_t(y) * width * 4], width * 4, &pair[std::size_t(y) * width * 8]);
                std::copy_n(&chrome.rgba[std::size_t(y) * width * 4], width * 4,
                            &pair[std::size_t(y) * width * 8 + width * 4]);
            }
            write_png(out / "image-scale.png", width * 2, height, pair);
            require(compared > 0, "cases compared");
            require(failed == 0, std::to_string(failed) + " of " + std::to_string(compared) +
                                     " cases differ from Chrome (" + (out / "image-scale.png").string() + ")");
            std::cout << "PASS image scaling on Metal against Chrome (" << compared << " cases)\n";
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "FAIL: " << e.what() << '\n';
            return 1;
        }
    }
}
