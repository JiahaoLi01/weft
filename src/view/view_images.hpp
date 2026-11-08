#pragma once

#include "base/result.hpp"
#include "paint/display_list.hpp"
#include "scene/scene.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The files a page names (docs/engine-communication.md section 8): background-image url() images, which the host reads
// by their paths, resolved against the page's base.
namespace weft {
    // Reads a file by its path; none when the host has no such file. Owner thread.
    using ViewResourceReader = std::function<std::optional<std::vector<std::uint8_t>>(std::string_view path)>;

    // A URL a page names, resolved against its base as RFC 3986 resolves a relative reference's path: what follows the
    // base's last '/' goes and the reference's path is merged in its place, its "." and ".." segments removed. A ".."
    // climbs no higher than the root of a rooted base ("/UI/", "C:/UI/"), and stays where a relative base has nothing
    // left to climb out of ("../../Content/UI/" + "../../x.png" is "../../../x.png"): the host reads relative paths
    // from where it is. A reference with a scheme ("data:", "https:", "C:") stays as written, one that starts with '/'
    // is from the host's root. A backslash is a slash; a query or a fragment ends the path.
    std::string view_resolve_url(std::string_view base, std::string_view reference);

    // A PNG file's pixels as straight sRGB RGBA8 (any PNG color type and depth, its colors taken as sRGB; ancillary
    // chunks ignored): Diag::SceneProfile for bytes that are no PNG, or an image of more than
    // constants::view::max_image_pixels pixels or a side over constants::view::max_image_side.
    Result<paint::Asset> view_decode_png(std::span<const std::uint8_t> bytes);

    // An image file's pixels as straight sRGB RGBA8, by what its bytes are (their signature, as Chrome sniffs an
    // image): PNG (view_decode_png), JPEG (libjpeg-turbo, as Blink decodes one: its Exif orientation applied, its ICC
    // profile not) or a still WebP (libwebp). Diag::SceneProfile for bytes that are none of those (a GIF, an SVG...),
    // and for an image past the bounds view_decode_png has, or translucent past
    // constants::view::max_translucent_image_pixels.
    Result<paint::Asset> view_decode_image(std::span<const std::uint8_t> bytes);

    // The images of one page load (SceneImages): each path read through the reader once, or a data: URL's bytes, and
    // decoded (view_decode_image). A path the reader has no file of, bytes that are no image the scene decodes, or an
    // image past the page's bounds is a diagnostic, once ("WEFT_SCENE_PROFILE: url(<path>): <why>"), and no image.
    class ViewImages final : public SceneImages {
    public:
        ViewImages(std::string base, const ViewResourceReader& reader, std::function<void(std::string)> diagnose);

        std::shared_ptr<const paint::Asset> image(std::string_view url) override;

        // The decoded bytes the page's images hold.
        std::size_t bytes() const {
            return bytes_;
        }

    private:
        std::shared_ptr<const paint::Asset> load(const std::string& path);

        std::string base_;
        const ViewResourceReader& reader_;
        std::function<void(std::string)> diagnose_;
        // By resolved path; none for a path that has no image.
        std::map<std::string, std::shared_ptr<const paint::Asset>, std::less<>> images_;
        std::size_t bytes_{};
    };
} // namespace weft
