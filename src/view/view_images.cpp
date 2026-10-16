#include "view/view_images.hpp"

#include "base/diagnostic.hpp"
#include "constants/common.hpp"
#include "constants/view.hpp"

#include <lodepng.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>

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
            out.width = width;
            out.height = height;
            out.encoding = paint::Encoding::SrgbStraight;
            out.bytes.assign(cleanup.pixels,
                             cleanup.pixels + std::size_t(width) * height * constants::rgba8_bytes_per_pixel);
            return std::nullopt;
        }
    } // namespace

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
        const auto fail = [&](const std::string& why) {
            diagnose_(make_diagnostic(Diag::SceneProfile, "url(" + path + "): " + why).message);
            return nullptr;
        };
        if (images_.size() >= constants::view::max_images) {
            return fail("past the page's " + std::to_string(constants::view::max_images) + " images");
        }
        if (!reader_) {
            return fail("no resource reader");
        }
        auto file = reader_(path);
        if (!file) {
            return fail("no such file");
        }
        if (file->size() > constants::view::max_resource_bytes) {
            return fail("a file of more than " + std::to_string(constants::view::max_resource_bytes) + " bytes");
        }
        paint::Asset decoded;
        if (const auto why = decode_png(*file, decoded)) {
            return fail(*why);
        }
        if (bytes_ + decoded.bytes.size() > constants::view::max_image_bytes) {
            return fail("past the page's " + std::to_string(constants::view::max_image_bytes) + " bytes of images");
        }
        bytes_ += decoded.bytes.size();
        return std::make_shared<const paint::Asset>(std::move(decoded));
    }
} // namespace weft
