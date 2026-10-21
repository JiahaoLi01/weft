// The logo pages (examples/logo) drawn by the native Metal pass against Chrome's frames of their animation, as
// logo_browser_reference checks the D3D11 pass (tests/logo_reference_tests.cpp): each variant's page.json loaded into a
// View of the cases' size, advanced to each case frame (1/60 s each), drawn by the Metal scene renderer's own queue and
// read back, against tools/browser_logo_reference.mjs's frames (tests/fixtures/logo-reference). The same measure: at
// most 0.5% of a frame's pixels differ by more than 32 levels in a channel. Side-by-side pairs (weft on Metal left,
// Chrome right) go to the output folder. Objective-C++ (ARC); PNGs through ImageIO.
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
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
    using weft::testing::require;
    namespace fs = std::filesystem;

    constexpr int level = 32;
    constexpr double share = 0.005;

    struct Rgba {
        unsigned width{};
        unsigned height{};
        std::vector<std::uint8_t> rgba;
    };

    // A PNG's pixels as sRGB RGBA8 rows (the references are opaque).
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

    struct Measure {
        double over{};
        double mean{};
        int max{};
    };

    // weft's frame (encoded premultiplied BGRA rows over a transparent target; the logo's root is opaque) against
    // Chrome's RGBA screenshot.
    Measure compare(const std::vector<std::uint8_t>& bgra, const Rgba& chrome) {
        Measure m;
        std::size_t over{};
        const auto pixels = std::size_t(chrome.width) * chrome.height;
        for (std::size_t i = 0; i < pixels; ++i) {
            int worst{};
            for (int c = 0; c < 3; ++c) {
                worst = std::max(worst, std::abs(int(bgra[i * 4 + 2 - c]) - int(chrome.rgba[i * 4 + c])));
            }
            over += worst > level;
            m.mean += worst;
            m.max = std::max(m.max, worst);
        }
        m.over = double(over) / double(pixels);
        m.mean /= double(pixels);
        return m;
    }

    // weft on the left, Chrome on the right.
    std::vector<std::uint8_t> pair(const std::vector<std::uint8_t>& bgra, const Rgba& chrome) {
        const auto w = chrome.width;
        const auto h = chrome.height;
        std::vector<std::uint8_t> out(std::size_t(w) * 2 * h * 4);
        for (unsigned y = 0; y < h; ++y) {
            for (unsigned x = 0; x < w; ++x) {
                const auto from = (std::size_t(y) * w + x) * 4;
                const auto left = (std::size_t(y) * w * 2 + x) * 4;
                const auto right = left + std::size_t(w) * 4;
                out[left] = bgra[from + 2];
                out[left + 1] = bgra[from + 1];
                out[left + 2] = bgra[from];
                out[left + 3] = 255;
                std::copy_n(&chrome.rgba[from], 4, &out[right]);
                out[right + 3] = 255;
            }
        }
        return out;
    }
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            const auto out = fs::path(argc > 1 ? argv[1] : "artifacts/logo-reference-metal");
            fs::create_directories(out);
            const auto logo = weft::testing::source_path("examples/logo");
            const auto fixtures = weft::testing::source_path("tests/fixtures/logo-reference");
            const auto cases = weft::testing::parse_json(weft::testing::read_file(logo / "reference-cases.json"));
            const auto width = unsigned(cases["width"].number);
            const auto height = unsigned(cases["height"].number);
            // The logo is strokes only: no font.
            const auto fonts = std::make_shared<weft::FontCatalog>();
            weft::SceneText text{*fonts};
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            require(device != nil, "a Metal device");
            weft::render::metal::SceneRenderer renderer(device, text);
            int failed{};
            int compared{};
            for (const auto& variant : cases["variants"].array) {
                const auto folder = logo / variant.string;
                const auto manifest = weft::testing::parse_json(weft::testing::read_file(folder / "page.json"));
                weft::ViewPage page;
                for (const auto& script : manifest["scripts"].array) {
                    page.scripts.push_back({script.string, weft::testing::read_text_file(folder / script.string)});
                }
                page.stylesheet = weft::testing::read_text_file(folder / manifest["stylesheet"].string);
                page.keyframes = weft::testing::read_text_file(folder / manifest["keyframes"].string);
                page.base = (folder / manifest["stylesheet"].string).generic_string();
                weft::View view{fonts, text, float(width), float(height), 1};
                view.load(page);
                view.advance(0);
                for (const auto& frame : cases["frames"].array) {
                    const auto at = int(frame.number);
                    view.advance(at * 1000.0 / 60.0);
                    const auto diagnostics = view.take_diagnostics();
                    require(diagnostics.empty(),
                            variant.string + ": " + (diagnostics.empty() ? std::string() : diagnostics.front().message));
                    const auto ours = renderer.draw(view.scene(), 1, width, height, std::nullopt, 0, {});
                    require(ours.has_value(), "native Metal pass: " + renderer.last_failure());
                    std::ostringstream name;
                    name << variant.string << '-' << std::setw(3) << std::setfill('0') << at;
                    const auto chrome = read_png(fixtures / (name.str() + ".png"));
                    require(chrome.width == width && chrome.height == height, name.str() + ": the reference's size");
                    const auto m = compare(*ours, chrome);
                    const bool passed = m.over <= share;
                    ++compared;
                    failed += !passed;
                    write_png(out / (name.str() + ".png"), width * 2, height, pair(*ours, chrome));
                    std::cout << (passed ? "ok   " : "FAIL ") << name.str() << ": " << std::fixed
                              << std::setprecision(3) << m.over * 100 << "% over " << level << ", mean "
                              << std::setprecision(2) << m.mean << ", max " << m.max << '\n';
                }
            }
            require(compared > 0, "frames compared");
            require(failed == 0,
                    std::to_string(failed) + " of " + std::to_string(compared) + " frames differ from Chrome");
            std::cout << "PASS logo reference on Metal (" << compared << " frames, " << renderer.draws << " draws)\n";
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "FAIL: " << e.what() << '\n';
            return 1;
        }
    }
}
