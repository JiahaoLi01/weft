#include "constants/text.hpp"
#include "scene/scene_text.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "support/json.hpp"
#include "support/scene_fonts.hpp"

#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Portable scene text on this machine's fonts: writes width, height, baseline and the faces used for the case
// matrix of tools/browser_text_metrics_reference.mjs (same order); tools/compare_scene_text.py compares the
// report with Chrome's tests/fixtures/scene_text_reference.json.
namespace {
    struct Case {
        std::string family;
        int weight{};
        int size{};
        std::string text;
        std::string letter_spacing;
        std::string line_height;
    };

    std::vector<Case> cases() {
        const std::vector<std::string> families{
            "system-ui",
            "sans-serif",
            "serif",
            "monospace",
            "\"Consolas\", monospace",
            "\"Segoe UI\", system-ui, sans-serif",
            "ui-sans-serif, system-ui, sans-serif, \"Apple Color Emoji\"",
            "\"Noto Serif SC\", \"Cinzel\", \"Times New Roman\", serif",
            "\"Cinzel Decorative\", \"EB Garamond\", \"Shippori Mincho\", \"LXGW NeoZhiSong Plus\", \"Noto Serif SC\", "
            "\"Times New Roman\", serif",
            "\"Noto Sans SC\", sans-serif",
            "Arial",
        };
        const std::vector<std::string> texts{"Hello World 123", "AVATAR Wave, fly!", "进度 42%",
                                             "Level 7 · 战斗",  "mono_case()",       "☠️"};
        std::vector<Case> out;
        for (const auto& family : families) {
            for (int weight : {400, 600, 700}) {
                for (int size : {10, 14, 16, 24}) {
                    for (const auto& text : texts) {
                        out.push_back({family, weight, size, text, "0", "normal"});
                    }
                }
            }
        }
        const std::vector<std::pair<std::string, std::string>> variants{
            {"0.1em", "normal"}, {"2px", "1.5"}, {"-0.5px", "20px"}, {"0", "1.25"}, {"0.05em", "24px"}};
        for (const auto* family :
             {"system-ui", "\"Consolas\", monospace", "\"Noto Serif SC\", \"Cinzel\", \"Times New Roman\", serif"}) {
            for (const auto& [spacing, line] : variants) {
                for (int size : {12, 16}) {
                    out.push_back({family, 400, size, "Hello World 123", spacing, line});
                    out.push_back({family, 700, size, "进度 42%", spacing, line});
                }
            }
        }
        return out;
    }
} // namespace

int main(int argc, char** argv) {
    try {
        weft::testing::require(argc == 2, "usage: weft_scene_text_tests <native.json>");
        const auto catalog = weft::testing::chrome_windows_font_catalog();
        std::map<weft::FontFaceSource, std::set<std::string>> names;
        std::map<weft::FontFaceSource, int> weights;
        for (const auto& face : catalog.faces) {
            names[face.source].insert(face.family);
            weights[face.source] = face.weight;
        }
        weft::SceneText text(catalog);
        std::ostringstream report;
        report.precision(9);
        report << "{\"cases\":[";
        const auto all = cases();
        for (std::size_t i = 0; i < all.size(); ++i) {
            const auto& c = all[i];
            weft::SceneStyle style{{"font-family", c.family},
                                   {"font-weight", std::to_string(c.weight)},
                                   {"font-size", std::to_string(c.size) + "px"},
                                   {"letter-spacing", c.letter_spacing},
                                   {"line-height", c.line_height}};
            const auto run = text.shape(c.text, style);
            std::set<std::uint32_t> faces;
            bool synthetic{};
            for (const auto& g : run->glyphs) {
                faces.insert(g.face);
                // CSS font-synthesis: Chrome emboldens a face lighter than 600 for a bold request.
                synthetic = synthetic || (c.weight >= 600 && weights[text.face_source(g.face)] < 600);
            }
            report << (i ? "," : "") << "{\"family\":" << weft::testing::json_string(c.family)
                   << ",\"weight\":" << c.weight << ",\"size\":" << c.size
                   << ",\"text\":" << weft::testing::json_string(c.text)
                   << ",\"letterSpacing\":" << weft::testing::json_string(c.letter_spacing)
                   << ",\"lineHeight\":" << weft::testing::json_string(c.line_height)
                   << ",\"width\":" << run->metrics.width << ",\"height\":" << run->metrics.height
                   << ",\"baseline\":" << run->metrics.baseline
                   << ",\"syntheticBold\":" << (synthetic ? "true" : "false") << ",\"faces\":[";
            bool first = true;
            for (auto face : faces) {
                report << (first ? "" : ",") << "[";
                bool first_name = true;
                for (const auto& name : names[text.face_source(face)]) {
                    report << (first_name ? "" : ",") << weft::testing::json_string(name);
                    first_name = false;
                }
                report << "]";
                first = false;
            }
            report << "]}";
        }
        report << "]}\n";

        // SDF stays available on the new face kinds: a TrueType collection face and variable-font instances.
        const auto sdf_inside = [&](const char* families, int weight, std::string_view glyph_text) {
            weft::SceneStyle style{{"font-family", families},
                                   {"font-weight", std::to_string(weight)},
                                   {"font-size", std::to_string(weft::constants::text::sdf_default_base_ppem) + "px"}};
            const auto run = text.shape(glyph_text, style);
            weft::testing::require(!run->glyphs.empty(), "SDF probe text shaped");
            const auto& g = run->glyphs.front();
            auto& font = text.face(g.face);
            const auto sdf = font.rasterize_sdf(g.glyph_id);
            const auto coverage = font.rasterize(g.glyph_id, float(weft::constants::text::sdf_default_base_ppem));
            weft::testing::require(sdf.width && sdf.height &&
                                       sdf.distances.size() == std::size_t(sdf.width) * sdf.height,
                                   "SDF bitmap of a scene face");
            // The field pads the coverage box by the spread on every side.
            weft::testing::require(sdf.width >= coverage.width && sdf.height >= coverage.height,
                                   "SDF covers the coverage glyph box");
            std::size_t inside{};
            for (auto d : sdf.distances) {
                inside += d >= weft::constants::text::sdf_zero_distance_byte;
            }
            weft::testing::require(inside > 0, "SDF has interior distances");
            return inside;
        };
        (void)sdf_inside("\"Microsoft YaHei UI\"", 400, "进");
        const auto regular = sdf_inside("\"Noto Serif SC\"", 400, "W");
        const auto bold = sdf_inside("\"Noto Serif SC\"", 700, "W");
        weft::testing::require(bold > regular, "variable font weight reaches the SDF outlines");

        std::filesystem::create_directories(std::filesystem::path(argv[1]).parent_path());
        weft::testing::write_file(argv[1], report.str());
        const auto stats = text.stats();
        std::cout << "PASS scene text: " << all.size() << " cases, " << stats.faces << " faces from " << stats.files
                  << " font files; SDF on a TTC face and two variable instances (interior " << regular << " -> " << bold
                  << " texels)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
