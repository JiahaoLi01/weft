#include "view/view_images.hpp"

#include "base/diagnostic.hpp"
#include "constants/common.hpp"
#include "constants/view.hpp"

#include "view/view_image_codecs.hpp"

#include <lodepng.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>
#include <variant>

namespace weft {
    namespace {
        // A path's segments, "." and ".." removed as RFC 3986 section 5.2.4 removes them, but for a relative path:
        // there a ".." with nothing left to climb out of stays (the host reads paths relative to where it is, as
        // "../../Content/UI"), while a rooted path's goes. Whether the path ends with a segment that names a directory
        // ("", "." or "..") is kept: it ends with '/'.
        std::string remove_dot_segments(std::string_view path, bool rooted) {
            std::vector<std::string_view> kept;
            bool directory{};
            std::size_t start{};
            while (start <= path.size()) {
                const auto end = std::min(path.find('/', start), path.size());
                const auto segment = path.substr(start, end - start);
                const bool last = end == path.size();
                if (segment == "..") {
                    if (!kept.empty() && kept.back() != "..") {
                        kept.pop_back();
                    } else if (!rooted) {
                        kept.push_back(segment);
                    }
                    directory = last;
                } else if (segment == ".") {
                    directory = last;
                } else if (!segment.empty() || last) {
                    kept.push_back(segment);
                    directory = false;
                }
                start = end + 1;
            }
            std::string out;
            for (std::size_t i = 0; i < kept.size(); ++i) {
                out.append(i ? "/" : "").append(kept[i]);
            }
            if (directory && !out.empty()) {
                out += '/';
            }
            return out;
        }

        // Whether a reference starts with a scheme: a letter, then letters, digits, '+', '-' or '.', then ':'. A
        // Windows drive ("C:") is a scheme too: such a reference is the host's to read.
        bool has_scheme(std::string_view reference) {
            const auto colon = reference.find(':');
            if (colon == reference.npos || colon == 0 || !std::isalpha(static_cast<unsigned char>(reference[0]))) {
                return false;
            }
            for (std::size_t i = 1; i < colon; ++i) {
                const auto ch = static_cast<unsigned char>(reference[i]);
                if (!std::isalnum(ch) && ch != '+' && ch != '-' && ch != '.') {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    std::string view_resolve_url(std::string_view base_text, std::string_view reference_text) {
        // A backslash is a slash, as browsers read one in a file or http URL.
        auto base = std::string(base_text);
        auto reference = std::string(reference_text.substr(0, reference_text.find_first_of("?#")));
        std::replace(base.begin(), base.end(), '\\', '/');
        std::replace(reference.begin(), reference.end(), '\\', '/');
        if (has_scheme(reference)) {
            return reference;
        }
        if (reference.starts_with('/')) {
            // The leading '/' stays: the path is the host's from its root.
            return "/" + remove_dot_segments(std::string_view(reference).substr(1), true);
        }
        const auto slash = base.rfind('/');
        const auto directory = slash == base.npos ? std::string_view{} : std::string_view(base).substr(0, slash + 1);
        const bool rooted = directory.starts_with('/');
        auto merged = std::string(directory.substr(rooted ? 1 : 0)).append(reference);
        // A base with a scheme or a drive ("C:/UI/") keeps it: ".." stops below it.
        std::string prefix;
        if (has_scheme(merged)) {
            const auto after = merged.find_first_not_of('/', merged.find(':') + 1);
            prefix = merged.substr(0, after == merged.npos ? merged.size() : after);
            merged.erase(0, prefix.size());
        }
        return (rooted ? "/" : "") + prefix + remove_dot_segments(merged, rooted || !prefix.empty());
    }

    namespace {
        // A PNG file's pixels into out (view_decode_png); why not, on failure.
        std::optional<std::string> decode_png(std::span<const std::uint8_t> bytes, paint::Asset& out) {
            LodePNGState state;
            lodepng_state_init(&state);
            state.info_raw.colortype = LCT_RGBA;
            state.info_raw.bitdepth = 8;

            struct Cleanup {
                LodePNGState& state;
                unsigned char* pixels{};

                ~Cleanup() {
                    std::free(pixels);
                    lodepng_state_cleanup(&state);
                }
            } cleanup{state};

            unsigned width{};
            unsigned height{};
            // The header first: an image past the bounds is never inflated.
            if (const auto error = lodepng_inspect(&width, &height, &state, bytes.data(), bytes.size())) {
                return std::string("no PNG image: ") + lodepng_error_text(error);
            }
            if (!width || !height || width > constants::view::max_image_side ||
                height > constants::view::max_image_side ||
                std::size_t(width) * height > constants::view::max_image_pixels) {
                return "a PNG image of " + std::to_string(width) + " x " + std::to_string(height) +
                       " pixels, past the bounds";
            }
            if (const auto error =
                    lodepng_decode(&cleanup.pixels, &width, &height, &state, bytes.data(), bytes.size())) {
                return std::string("no PNG image: ") + lodepng_error_text(error);
            }
            const auto pixels = std::size_t(width) * height;
            if (pixels > constants::view::max_translucent_image_pixels) {
                for (std::size_t p = 3; p < pixels * constants::rgba8_bytes_per_pixel;
                     p += constants::rgba8_bytes_per_pixel) {
                    if (cleanup.pixels[p] != constants::color_channel_max) {
                        return "a translucent PNG image of " + std::to_string(width) + " x " + std::to_string(height) +
                               " pixels, past the bounds";
                    }
                }
            }
            out.width = width;
            out.height = height;
            out.encoding = paint::Encoding::SrgbStraight;
            out.bytes.assign(cleanup.pixels, cleanup.pixels + pixels * constants::rgba8_bytes_per_pixel);
            return std::nullopt;
        }
    } // namespace

    namespace {
        bool starts(std::span<const std::uint8_t> bytes, std::string_view prefix, std::size_t at = 0) {
            return bytes.size() >= at + prefix.size() &&
                   std::equal(prefix.begin(), prefix.end(), bytes.begin() + std::ptrdiff_t(at),
                              [](char a, std::uint8_t b) {
                                  return static_cast<std::uint8_t>(a) == b;
                              });
        }

        std::optional<std::string> within_bounds(const paint::Asset& out);

        // An image file's pixels into out by what its bytes are, as Chrome sniffs an image (its signature, not its
        // name or declared type): PNG, JPEG or WebP; why not, on failure, naming the formats the scene does not decode.
        std::optional<std::string> decode_image(std::span<const std::uint8_t> bytes, paint::Asset& out) {
            std::optional<std::string> why;
            if (starts(bytes, "\x89PNG\r\n\x1a\n")) {
                return decode_png(bytes, out);
            }
            if (starts(bytes, "\xff\xd8\xff")) {
                why = view_codecs::decode_jpeg(bytes, constants::view::max_image_side,
                                               constants::view::max_image_pixels, out);
            } else if (starts(bytes, "RIFF") && starts(bytes, "WEBP", 8)) {
                why = view_codecs::decode_webp(bytes, constants::view::max_image_side,
                                               constants::view::max_image_pixels, out);
            } else {
                // What it is, if the scene knows it: one Chrome decodes that the scene does not.
                std::size_t text = 0;
                while (text < bytes.size() &&
                       (bytes[text] == ' ' || bytes[text] == '\t' || bytes[text] == '\n' || bytes[text] == '\r' ||
                        bytes[text] == 0xef || bytes[text] == 0xbb || bytes[text] == 0xbf)) {
                    ++text;
                }
                const char* kind = starts(bytes, "GIF87a") || starts(bytes, "GIF89a")             ? "a GIF image"
                                   : starts(bytes, "BM")                                          ? "a BMP image"
                                   : starts(bytes, std::string_view("\0\0\1\0", 4))               ? "an ICO image"
                                   : starts(bytes, "ftypavif", 4) || starts(bytes, "ftypavis", 4) ? "an AVIF image"
                                   : starts(bytes, "<svg", text) || starts(bytes, "<?xml", text)  ? "an SVG image"
                                                                                                  : "no image";
                return std::string(kind) + " (the scene decodes PNG, JPEG and WebP)";
            }
            if (why) {
                return why;
            }
            return within_bounds(out);
        }

        // An image within the translucent bound; why not.
        std::optional<std::string> within_bounds(const paint::Asset& out) {
            const auto pixels = std::size_t(out.width) * out.height;
            if (pixels > constants::view::max_translucent_image_pixels) {
                for (std::size_t p = 3; p < pixels * constants::rgba8_bytes_per_pixel;
                     p += constants::rgba8_bytes_per_pixel) {
                    if (out.bytes[p] != constants::color_channel_max) {
                        return "a translucent image of " + std::to_string(out.width) + " x " +
                               std::to_string(out.height) + " pixels, past the bounds";
                    }
                }
            }
            return std::nullopt;
        }

        // The bytes a data: URL holds (the Fetch standard's data: URL processor): its body after the first comma,
        // percent-decoded, then base64-decoded (forgiving: ASCII whitespace dropped, padding optional) when its type
        // ends with ;base64. Its media type is not read: the bytes say what image they are. Why not, on failure.
        std::variant<std::vector<std::uint8_t>, std::string> data_url(std::string_view url) {
            const auto comma = url.find(',');
            if (comma == url.npos) {
                return std::string("a data: URL without a comma");
            }
            auto type = std::string(url.substr(5, comma - 5));
            while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) {
                type.pop_back();
            }
            std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) {
                return char(std::tolower(c));
            });
            std::string body;
            body.reserve(url.size() - comma);
            for (std::size_t i = comma + 1; i < url.size(); ++i) {
                const auto hex = [&](std::size_t at) {
                    return at < url.size() && std::isxdigit(static_cast<unsigned char>(url[at]));
                };
                if (url[i] == '%' && hex(i + 1) && hex(i + 2)) {
                    body += char(std::stoi(std::string(url.substr(i + 1, 2)), nullptr, 16));
                    i += 2;
                } else {
                    body += url[i];
                }
            }
            auto base64 = type.size() >= 7 && type.ends_with("base64");
            if (base64) {
                // ";base64", spaces allowed before "base64".
                auto rest = std::string_view(type).substr(0, type.size() - 6);
                while (!rest.empty() && rest.back() == ' ') {
                    rest.remove_suffix(1);
                }
                base64 = rest.ends_with(';');
            }
            if (!base64) {
                return std::vector<std::uint8_t>(body.begin(), body.end());
            }
            std::string text;
            for (const char c : body) {
                if (c != ' ' && c != '\t' && c != '\n' && c != '\f' && c != '\r') {
                    text += c;
                }
            }
            if (text.size() % 4 == 0 && text.ends_with("==")) {
                text.resize(text.size() - 2);
            } else if (text.size() % 4 == 0 && text.ends_with('=')) {
                text.pop_back();
            }
            if (text.size() % 4 == 1) {
                return std::string("a data: URL whose base64 does not decode");
            }
            std::vector<std::uint8_t> out;
            out.reserve(text.size() * 3 / 4);
            std::uint32_t buffer{};
            int bits{};
            for (const char c : text) {
                int v = c >= 'A' && c <= 'Z'   ? c - 'A'
                        : c >= 'a' && c <= 'z' ? c - 'a' + 26
                        : c >= '0' && c <= '9' ? c - '0' + 52
                        : c == '+'             ? 62
                        : c == '/'             ? 63
                                               : -1;
                if (v < 0) {
                    return std::string("a data: URL whose base64 does not decode");
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

        bool data_scheme(std::string_view path) {
            return path.size() >= 5 && std::tolower(static_cast<unsigned char>(path[0])) == 'd' &&
                   std::tolower(static_cast<unsigned char>(path[1])) == 'a' &&
                   std::tolower(static_cast<unsigned char>(path[2])) == 't' &&
                   std::tolower(static_cast<unsigned char>(path[3])) == 'a' && path[4] == ':';
        }
    } // namespace

    Result<paint::Asset> view_decode_image(std::span<const std::uint8_t> bytes) {
        paint::Asset asset;
        if (const auto why = decode_image(bytes, asset); why && asset.bytes.empty()) {
            return make_diagnostic(Diag::SceneProfile, *why);
        }
        return asset;
    }

    Result<paint::Asset> view_decode_png(std::span<const std::uint8_t> bytes) {
        paint::Asset asset;
        if (const auto why = decode_png(bytes, asset)) {
            return make_diagnostic(Diag::SceneProfile, *why);
        }
        return asset;
    }

    ViewImages::ViewImages(std::string base, const ViewResourceReader& reader,
                           std::function<void(std::string)> diagnose)
        : base_(std::move(base)), reader_(reader), diagnose_(std::move(diagnose)) {
    }

    std::shared_ptr<const paint::Asset> ViewImages::image(std::string_view url) {
        const auto path = view_resolve_url(base_, url);
        if (const auto found = images_.find(path); found != images_.end()) {
            return found->second;
        }
        auto image = load(path);
        images_.emplace(path, image);
        return image;
    }

    std::shared_ptr<const paint::Asset> ViewImages::load(const std::string& path) {
        // A data: URL is named by its start in diagnostics.
        constexpr std::size_t named = 64;
        const auto fail = [&](const std::string& why) {
            const auto name = path.size() > named ? path.substr(0, named) + "..." : path;
            diagnose_(make_diagnostic(Diag::SceneProfile, "url(" + name + "): " + why).message);
            return nullptr;
        };
        if (images_.size() >= constants::view::max_images) {
            return fail("past the page's " + std::to_string(constants::view::max_images) + " images");
        }
        std::optional<std::vector<std::uint8_t>> file;
        if (data_scheme(path)) {
            if (path.size() > constants::view::max_resource_bytes * 2) {
                return fail("a data: URL of more than " + std::to_string(constants::view::max_resource_bytes * 2) +
                            " characters");
            }
            auto bytes = data_url(path);
            if (const auto* why = std::get_if<std::string>(&bytes)) {
                return fail(*why);
            }
            file = std::move(std::get<std::vector<std::uint8_t>>(bytes));
        } else {
            if (!reader_) {
                return fail("no resource reader");
            }
            file = reader_(path);
            if (!file) {
                return fail("no such file");
            }
        }
        if (file->size() > constants::view::max_resource_bytes) {
            return fail("a file of more than " + std::to_string(constants::view::max_resource_bytes) + " bytes");
        }
        paint::Asset decoded;
        if (const auto why = decode_image(*file, decoded)) {
            if (decoded.bytes.empty()) {
                return fail(*why);
            }
            // Its size without its pixels: a diagnostic, and the image drawn as nothing.
            fail(*why);
        }
        if (bytes_ + decoded.bytes.size() > constants::view::max_image_bytes) {
            return fail("past the page's " + std::to_string(constants::view::max_image_bytes) + " bytes of images");
        }
        bytes_ += decoded.bytes.size();
        return std::make_shared<const paint::Asset>(std::move(decoded));
    }
} // namespace weft
