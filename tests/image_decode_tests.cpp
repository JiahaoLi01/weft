#include "support/check.hpp"
#include "support/files.hpp"
#include "support/json_value.hpp"
#include "view/view_images.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// Image decoding against Chrome (tools/browser_image_decode_reference.mjs, tests/fixtures/image_decode): each image's
// natural size and pixels as weft decodes it (view_decode_image: PNG, JPEG, WebP by their bytes) against Chrome's, drawn
// at its natural size into a canvas. An opaque pixel is the same bytes; a translucent one, read back from Chrome's
// premultiplied canvas, the same premultiplied to a level. The same images as data: URLs (base64, and one
// percent-encoded) give the same pixels through ViewImages, and bytes that are no image the scene decodes say what
// they are. Portable.
namespace {
    using weft::testing::JsonValue;
    using weft::testing::require;

    const std::string directory = WEFT_SOURCE_DIR "/tests/fixtures/image_decode/";

    std::vector<std::uint8_t> bytes_of(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        require(bool(in), "reads " + path);
        return {std::istreambuf_iterator<char>(in), {}};
    }

    std::vector<std::uint8_t> base64_decode(const std::string& text) {
        std::vector<std::uint8_t> out;
        std::uint32_t buffer{};
        int bits{};
        for (const char c : text) {
            const int v = c >= 'A' && c <= 'Z'   ? c - 'A'
                          : c >= 'a' && c <= 'z' ? c - 'a' + 26
                          : c >= '0' && c <= '9' ? c - '0' + 52
                          : c == '+'             ? 62
                          : c == '/'             ? 63
                                                 : -1;
            if (v < 0) {
                continue;
            }
            buffer = (buffer << 6) | std::uint32_t(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(std::uint8_t((buffer >> bits) & 0xff));
            }
        }
        return out;
    }

    std::string base64_encode(const std::vector<std::uint8_t>& bytes) {
        static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (std::size_t i = 0; i < bytes.size(); i += 3) {
            const std::uint32_t n = std::uint32_t(bytes[i]) << 16 |
                                    (i + 1 < bytes.size() ? std::uint32_t(bytes[i + 1]) << 8 : 0) |
                                    (i + 2 < bytes.size() ? bytes[i + 2] : 0);
            out += alphabet[n >> 18 & 63];
            out += alphabet[n >> 12 & 63];
            out += i + 1 < bytes.size() ? alphabet[n >> 6 & 63] : '=';
            out += i + 2 < bytes.size() ? alphabet[n & 63] : '=';
        }
        return out;
    }

    // How far two RGBA8 images are apart: the largest difference of a channel of an opaque pixel, and of a
    // premultiplied channel (and alpha) of a translucent one; and how many pixels differ at all.
    struct Difference {
        int opaque{};
        int translucent{};
        std::size_t pixels{};
    };

    Difference compare(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
        Difference d;
        for (std::size_t p = 0; p + 3 < a.size() && p + 3 < b.size(); p += 4) {
            bool differs{};
            if (a[p + 3] == 255 && b[p + 3] == 255) {
                for (int k = 0; k < 3; ++k) {
                    const int v = std::abs(int(a[p + k]) - int(b[p + k]));
                    d.opaque = std::max(d.opaque, v);
                    differs |= v != 0;
                }
            } else {
                const auto premultiplied = [](const std::vector<std::uint8_t>& x, std::size_t at, int k) {
                    return k == 3 ? int(x[at + 3]) : int(std::lround(x[at + std::size_t(k)] * x[at + 3] / 255.0));
                };
                for (int k = 0; k < 4; ++k) {
                    const int v = std::abs(premultiplied(a, p, k) - premultiplied(b, p, k));
                    d.translucent = std::max(d.translucent, v);
                    differs |= v != 0;
                }
            }
            d.pixels += differs ? 1 : 0;
        }
        return d;
    }
} // namespace

int main() {
    try {
        const auto fixture = weft::testing::parse_json(weft::testing::read_file(directory + "chrome-results.json"));
        // Where weft differs on purpose: none (an animated WebP shows its first frame, which Chrome draws before it
        // plays, and does not play).
        const std::map<std::string, std::string> known;
        std::vector<std::string> failures;
        std::vector<std::string> differences;
        std::size_t decoded{};
        std::size_t pixels{};
        int largest_translucent{};
        std::map<std::string, std::vector<std::uint8_t>> weft_pixels;
        for (const auto& image : fixture["images"].array) {
            const auto& name = image["name"].string;
            const auto file = bytes_of(directory + name);
            const auto result = weft::view_decode_image(file);
            if (const auto* error = image.find("error")) {
                if (result) {
                    failures.push_back(name + ": decoded, Chrome gave " + error->string);
                }
                continue;
            }
            if (!result) {
                const auto why = std::string(result.error().message);
                if (const auto k = known.find(name); k != known.end()) {
                    differences.push_back(name + " (" + k->second + "): " + why);
                } else {
                    failures.push_back(name + ": not decoded: " + why);
                }
                continue;
            }
            const auto& asset = *result;
            const auto want_w = std::uint32_t(image["width"].number);
            const auto want_h = std::uint32_t(image["height"].number);
            if (asset.width != want_w || asset.height != want_h) {
                std::ostringstream m;
                m << name << ": " << asset.width << " x " << asset.height << ", Chrome " << want_w << " x " << want_h;
                failures.push_back(m.str());
                continue;
            }
            const auto chrome = base64_decode(image["rgba"].string);
            require(chrome.size() == asset.bytes.size(), name + ": Chrome's pixels");
            const auto d = compare(asset.bytes, chrome);
            largest_translucent = std::max(largest_translucent, d.translucent);
            if (d.opaque > 0 || d.translucent > 1) {
                std::ostringstream m;
                m << name << ": " << d.pixels << " pixels differ, by up to " << d.opaque << " (opaque) and "
                  << d.translucent << " (translucent, premultiplied)";
                failures.push_back(m.str());
            }
            weft_pixels[name] = asset.bytes;
            ++decoded;
            pixels += std::size_t(asset.width) * asset.height;
        }

        // data: URLs through ViewImages: each decoded image again as base64, one PNG percent-encoded; then what is no
        // image the scene decodes.
        std::vector<std::string> diagnostics;
        const weft::ViewResourceReader no_files = [](std::string_view) {
            return std::optional<std::vector<std::uint8_t>>{};
        };
        weft::ViewImages images("", no_files, [&](std::string m) {
            diagnostics.push_back(std::move(m));
        });
        std::size_t urls{};
        for (const auto& [name, expected] : weft_pixels) {
            const auto type = name.ends_with(".webp") ? "image/webp" : "image/jpeg";
            const auto url = std::string("data:") + type + ";base64," + base64_encode(bytes_of(directory + name));
            const auto asset = images.image(url);
            if (!asset || asset->bytes != expected) {
                failures.push_back(name + " as a data: URL: not the same pixels");
            }
            ++urls;
        }
        {
            // A 1 x 1 PNG percent-encoded, its declared type another: the bytes say what it is.
            const std::vector<std::uint8_t> png = bytes_of(WEFT_SOURCE_DIR "/tests/fixtures/img/dot-1x1.png");
            std::string encoded;
            for (const auto b : png) {
                static constexpr char hex[] = "0123456789ABCDEF";
                encoded += '%';
                encoded += hex[b >> 4];
                encoded += hex[b & 15];
            }
            const auto asset = images.image("data:text/plain," + encoded);
            const auto file = weft::view_decode_image(png);
            require(asset && file && asset->bytes == file->bytes,
                    "a percent-encoded data: URL of another declared type");
            ++urls;
        }
        const auto expect = [&](const std::string& url, const std::string& says) {
            diagnostics.clear();
            require(!images.image(url), url.substr(0, 40) + ": no image");
            require(diagnostics.size() == 1 && diagnostics[0].find(says) != std::string::npos,
                    url.substr(0, 40) + ": says " + says + " (" + (diagnostics.empty() ? "" : diagnostics[0]) + ")");
        };
        expect("data:image/png;base64", "without a comma");
        expect("data:image/png;base64,a", "base64 does not decode");
        expect("data:image/png;base64,ab*d", "base64 does not decode");
        expect("data:image/gif;base64,R0lGODlhAQABAAAAACw=", "a GIF image");
        expect("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg'/%3E", "an SVG image");
        expect("data:,hello", "no image");
        expect("data:image/jpeg;base64," + base64_encode(std::vector<std::uint8_t>{0xff, 0xd8, 0xff, 0xe0, 0, 2}),
               "no JPEG image");
        expect("data:image/webp;base64," +
                   base64_encode(std::vector<std::uint8_t>{'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'E', 'B', 'P'}),
               "no WebP image");

        std::cout << fixture["images"].size() << " images vs " << fixture["browser"].string << ": " << decoded
                  << " decoded alike (" << pixels << " pixels; opaque pixels the same bytes, translucent ones within "
                  << largest_translucent << " level premultiplied), " << urls << " data: URLs; " << failures.size()
                  << " failures\n";
        for (const auto& d : differences) {
            std::cout << "  known: " << d << '\n';
        }
        for (const auto& f : failures) {
            std::cout << "  " << f << '\n';
        }
        require(failures.empty(), "images decode as Chrome decodes them");
        std::cout << "PASS image decoding against Chrome\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
