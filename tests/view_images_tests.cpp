#define NOMINMAX
#include "view/view.hpp"
#include "view/view_images.hpp"
#include "support/check.hpp"
#include "support/scene_fonts.hpp"

#include <array>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// A page's url() images (src/view/view_images.hpp, docs/engine-communication.md section 8): URLs resolved against the
// page's base, PNG files decoded within their bounds, each path read once a page load, what fails a diagnostic once,
// and a View's scene showing the image its reader gave.
namespace {
    using weft::testing::require;

    // 2 x 2 RGBA8: red, half green, clear blue, a quarter of white.
    const std::vector<std::uint8_t> rgba_png{
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
        0x17, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x9f, 0xe1, 0x3f, 0x43, 0x03, 0x23,
        0x98, 0xfe, 0xcf, 0xe0, 0x00, 0x00, 0x3d, 0x23, 0x06, 0xbc, 0x3a, 0x2b, 0xe2, 0xc0, 0x00, 0x00, 0x00, 0x00,
        0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    // 3 x 1 palette: (10, 20, 30), (200, 100, 50), and index 2 transparent.
    const std::vector<std::uint8_t> palette_png{
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x00, 0x00, 0x00, 0x66, 0x8e, 0xfc, 0x27, 0x00, 0x00, 0x00,
        0x09, 0x50, 0x4c, 0x54, 0x45, 0x0a, 0x14, 0x1e, 0xc8, 0x64, 0x32, 0x00, 0x00, 0x00, 0x12, 0x76, 0xc6, 0x62,
        0x00, 0x00, 0x00, 0x03, 0x74, 0x52, 0x4e, 0x53, 0xff, 0xff, 0x00, 0xd7, 0xca, 0x0d, 0x41, 0x00, 0x00, 0x00,
        0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x90, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x19, 0x2d, 0x88, 0xf4,
        0x36, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    // 1 x 1 16-bit gray 0x8000.
    const std::vector<std::uint8_t> gray16_png{
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xee, 0x47, 0x16, 0x00, 0x00, 0x00,
        0x0b, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x68, 0x60, 0x00, 0x00, 0x01, 0x03, 0x00, 0x81, 0x3e, 0x4c,
        0xc5, 0x93, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    // A signature and a header of 5000 x 5000 RGBA8, nothing more.
    const std::vector<std::uint8_t> huge_png{
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x13, 0x88, 0x00, 0x00, 0x13, 0x88, 0x08, 0x06, 0x00, 0x00, 0x00, 0x5d, 0x98, 0x87, 0xcb};

    void resolves() {
        const std::vector<std::array<std::string, 3>> cases{{
            {"UI/City/page.css", "../Textures/pigment.png", "UI/Textures/pigment.png"},
            {"UI/City/", "img.png", "UI/City/img.png"},
            {"", "a/./b/../c.png", "a/c.png"},
            {"UI/page.css", "../../../x.png", "../../x.png"},
            {"../../../Game/Content/UI/City/page.css", "../Textures/p.png", "../../../Game/Content/UI/Textures/p.png"},
            {"/game/UI/City/page.css", "../T/p.png", "/game/UI/T/p.png"},
            {"/UI/page.css", "../../x.png", "/x.png"},
            {"F:/Projects/UI/City/page.css", "../T/p.png", "F:/Projects/UI/T/p.png"},
            {"F:/UI/page.css", "../../x.png", "F:/x.png"},
            {"F:\\UI\\City\\page.css", "..\\T\\p.png", "F:/UI/T/p.png"},
            {"UI/City/page.css", "/abs/../x.png", "/x.png"},
            {"UI/City/page.css", "/../x.png", "/x.png"},
            {"UI/City/page.css", "data:image/png;base64,AAAA", "data:image/png;base64,AAAA"},
            {"UI/City/page.css", "img.png?v=2#frag", "UI/City/img.png"},
            {"UI/City/page.css", "dir/", "UI/City/dir/"},
            {"UI/City/page.css", "..", "UI/"},
        }};
        for (const auto& [base, reference, expected] : cases) {
            const auto resolved = weft::view_resolve_url(base, reference);
            require(resolved == expected, base + " + " + reference + ": " + resolved + ", not " + expected);
        }
    }

    void decodes() {
        const auto rgba = weft::view_decode_png(rgba_png);
        require(rgba && rgba->width == 2 && rgba->height == 2 && rgba->encoding == weft::paint::Encoding::SrgbStraight &&
                    rgba->bytes == std::vector<std::uint8_t>{255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 255, 255,
                                                             255, 64},
                "RGBA8 texels as written, straight");
        const auto palette = weft::view_decode_png(palette_png);
        require(palette && palette->width == 3 && palette->height == 1 &&
                    palette->bytes == std::vector<std::uint8_t>{10, 20, 30, 255, 200, 100, 50, 255, 0, 0, 0, 0},
                "a palette and its transparency");
        const auto gray = weft::view_decode_png(gray16_png);
        require(gray && gray->bytes == std::vector<std::uint8_t>{128, 128, 128, 255}, "16-bit gray to 8 bits");
        const auto huge = weft::view_decode_png(huge_png);
        require(!huge && huge.error().message.find("past the bounds") != std::string::npos,
                "a header past the bounds, never inflated");
        require(!weft::view_decode_png(std::vector<std::uint8_t>{1, 2, 3}), "bytes that are no PNG");
        require(!weft::view_decode_png(std::span(rgba_png).first(40)), "a truncated PNG");
        require(!weft::view_decode_png({}), "no bytes");
    }

    void reads_once() {
        std::map<std::string, int> reads;
        const std::map<std::string, std::vector<std::uint8_t>> files{{"ui/img/a.png", rgba_png},
                                                                      {"ui/img/bad.png", {1, 2, 3}}};
        weft::ViewResourceReader reader = [&](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
            ++reads[std::string(path)];
            const auto found = files.find(std::string(path));
            if (found == files.end()) {
                return std::nullopt;
            }
            return found->second;
        };
        std::vector<std::string> diagnosed;
        weft::ViewImages images("ui/page/page.css", reader, [&](std::string m) {
            diagnosed.push_back(std::move(m));
        });
        const auto a = images.image("../img/a.png");
        require(a && a->width == 2 && images.image("../img/./a.png") == a, "one asset for a path, however written");
        require(reads["ui/img/a.png"] == 1 && images.bytes() == a->bytes.size(), "a path read once");
        require(!images.image("/ui/img/a.png") && reads["/ui/img/a.png"] == 1, "a path from the host's root is another");
        require(!images.image("../img/none.png") && !images.image("../img/none.png") && reads["ui/img/none.png"] == 1,
                "a missing file, read once");
        require(!images.image("../img/bad.png") && reads["ui/img/bad.png"] == 1, "bytes that are no PNG");
        require(diagnosed.size() == 3 && diagnosed[1].find("url(ui/img/none.png): no such file") != std::string::npos &&
                    diagnosed[2].find("url(ui/img/bad.png): no PNG image") != std::string::npos,
                "each failure a diagnostic, once");
        const weft::ViewResourceReader none;
        weft::ViewImages unread("", none, [&](std::string m) {
            diagnosed.push_back(std::move(m));
        });
        require(!unread.image("a.png") && diagnosed.back().find("no resource reader") != std::string::npos,
                "no reader");
    }

    void shows_in_a_view() {
        const auto fonts = std::make_shared<weft::FontCatalog>(weft::testing::chrome_windows_font_catalog());
        weft::SceneText text{*fonts};
        weft::View view{fonts, text, 320, 200, 1};
        int reads{};
        view.set_resource_reader([&](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
            ++reads;
            if (path != "ui/img/dot.png") {
                return std::nullopt;
            }
            return rgba_png;
        });
        weft::ViewPage page;
        page.scripts.push_back({"page.js", "for (const id of ['a', 'b', 'c']) {"
                                           "  const d = document.createElement('div');"
                                           "  d.id = id;"
                                           "  document.getElementById('root').appendChild(d);"
                                           "}"});
        page.stylesheet = "#root{position:relative;width:100%;height:100%}"
                          "#a,#b,#c{position:absolute;top:0;width:8px;height:8px}"
                          "#a{left:0;background-image:url(../img/dot.png)}"
                          "#b{left:10px;background-image:url('../img/dot.png');background-size:100% 100%}"
                          "#c{left:20px;background:#102030 url( \"../img/dot.png\" ) no-repeat center}";
        page.base = "ui/page/page.css";
        view.load(page);
        const auto find = [&](std::string_view id) {
            const weft::SceneNode* found{};
            const auto visit = [&](auto&& self, const weft::SceneNode& n) -> void {
                if (const auto it = n.attributes.find("id"); it != n.attributes.end() && it->second == id) {
                    found = &n;
                }
                for (const auto& c : n.children) {
                    self(self, c);
                }
            };
            visit(visit, view.scene().root);
            require(found != nullptr, "the element " + std::string(id));
            return found;
        };
        const auto image = find("a")->background_image;
        require(image && image->width == 2 && find("b")->background_image == image &&
                    find("c")->background_image == image,
                "the reader's image on each element, however the url() is written");
        require(reads == 1 && view.take_diagnostics().empty(), "one read, no diagnostic");
        view.advance(16);
        require(reads == 1, "no read again in the same page load");
        view.load(page);
        require(reads == 2 && find("a")->background_image && find("a")->background_image != image,
                "a page load reads its files anew");
        page.stylesheet += "#a{background-image:url(missing.png)}";
        view.load(page);
        const auto diagnostics = view.take_diagnostics();
        require(!find("a")->background_image && diagnostics.size() == 1 &&
                    diagnostics[0].kind == weft::ViewDiagnostic::Kind::Scene &&
                    diagnostics[0].message.find("url(ui/page/missing.png): no such file") != std::string::npos,
                "a missing image shows nothing, a scene diagnostic");
        view.set_resource_reader({});
        view.load(page);
        require(!find("b")->background_image && !view.take_diagnostics().empty(), "without a reader, no image");
    }
} // namespace

int main() {
    try {
        resolves();
        decodes();
        reads_once();
        shows_in_a_view();
        std::cout << "PASS view images\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
