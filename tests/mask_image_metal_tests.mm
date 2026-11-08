// CSS masks drawn by the native Metal pass against Chrome: the page of tests/fixtures/mask_image (page.js builds boxes
// with a border, padding and an overflowing child, masked by gradient layers, cases.json) in a View of the reference's
// size, drawn by the Metal scene renderer and read back over white, against tools/browser_mask_image_reference.mjs's
// screenshot of the same page in Chrome (chrome.png), cell by cell. What it checks: the layers (scene_mask), their
// tiles, painting areas, modes and composites as a group's composite evaluates them (scene.hlsl's clipMaskCoverage),
// and that the mask multiplies the element's group, not each item (the opaque child over the opaque box shows none of
// the box through it). A cell passes when at most `share` of its pixels differ from Chrome's by more than `level` in a
// channel. The hits are tests/mask_image_tests.cpp's. A side-by-side image (weft on Metal left, Chrome right) goes to
// the output folder. Objective-C++ (ARC); PNGs through ImageIO.
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
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
    using weft::testing::require;
    namespace fs = std::filesystem;

    // The mask images by their paths, resolved against the page's base (the fixture folder).
    std::optional<std::vector<std::uint8_t>> read_resource(std::string_view path) {
        std::ifstream in(std::string(path), std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
    }

    // Channel levels past which a pixel differs, and the share of a cell's pixels that may.
    constexpr int level = 48;
    constexpr double share = 0.002;

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

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            const auto out = fs::path(argc > 1 ? argv[1] : "artifacts/mask-image-metal");
            fs::create_directories(out);
            const auto directory = weft::testing::source_path("tests/fixtures/mask_image");
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
            view.advance(16);
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
                std::size_t over{};
                double mean{};
                int max{};
                for (unsigned y = y0; y < y0 + h; ++y) {
                    for (unsigned x = x0; x < x0 + w; ++x) {
                        const auto i = (std::size_t(y) * width + x) * 4;
                        int worst{};
                        for (int k = 0; k < 3; ++k) {
                            worst = std::max(worst, std::abs(int(weft[i + k]) - int(chrome.rgba[i + k])));
                        }
                        over += worst > level;
                        mean += worst;
                        max = std::max(max, worst);
                    }
                }
                const double pixels = double(w) * h;
                const bool passed = double(over) / pixels <= share;
                ++compared;
                failed += !passed;
                std::cout << (passed ? "ok   " : "FAIL ") << std::left << std::setw(26) << c["name"].string
                          << std::right << std::fixed << std::setprecision(2) << " mean " << mean / pixels << ", max "
                          << std::setw(3) << max << ", " << over << " over " << level << '\n';
            }

            // A change inside a masked element patches the kept pass where the change lies (its group's composite
            // keeps the mask's entries): the drawing equals a fresh renderer's of the same frame.
            {
                const auto& cases = manifest["cases"].array;
                std::size_t layered{};
                while (layered < cases.size() && cases[layered].find("name")->string != "two layers add") {
                    ++layered;
                }
                require(layered < cases.size(), "the two-layer case");
                const auto [ok, result] = view.evaluate("document.getElementById('child" + std::to_string(layered) +
                                                        "').style.background = '#16a34a'");
                require(ok, "the child's color changes: " + result);
                view.advance(32);
                const auto patches = renderer.kept_patches;
                const auto again = renderer.draw(view.scene(), 1, width, height, std::nullopt, 0, {});
                weft::render::metal::SceneRenderer fresh(device, text);
                const auto whole = fresh.draw(view.scene(), 1, width, height, std::nullopt, 0, {});
                require(again && whole, "both renderers draw the changed frame");
                std::cout << "patched " << renderer.kept_patches - patches << " time(s)";
                for (const auto& [why, n] : renderer.patch_refusals) {
                    std::cout << ", refused " << n << ": " << why;
                }
                std::cout << '\n';
                require(*again == *whole, "the patched drawing equals a fresh one");
            }

            std::vector<std::uint8_t> pair(std::size_t(width) * 2 * height * 4);
            for (unsigned y = 0; y < height; ++y) {
                std::copy_n(&weft[std::size_t(y) * width * 4], width * 4, &pair[std::size_t(y) * width * 8]);
                std::copy_n(&chrome.rgba[std::size_t(y) * width * 4], width * 4,
                            &pair[std::size_t(y) * width * 8 + width * 4]);
            }
            write_png(out / "mask-image.png", width * 2, height, pair);
            require(compared > 0, "cases compared");
            require(failed == 0, std::to_string(failed) + " of " + std::to_string(compared) +
                                     " cells differ from Chrome (" + (out / "mask-image.png").string() + ")");
            std::cout << "PASS masks on Metal against Chrome (" << compared << " cells)\n";
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "FAIL: " << e.what() << '\n';
            return 1;
        }
    }
}
