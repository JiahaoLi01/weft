#include "layout/document.hpp"
#include "script/script_platform.hpp"
#include "script/script_dom.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "constants/layout.hpp"
#include "test_constants/layout.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace weft;

namespace {
    using weft::testing::read_text_file;
    using weft::testing::require;

} // namespace

int main(int argc, char** argv) {
    try {
        FontSet fonts({WEFT_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf",
                       WEFT_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf"});
        ScriptDom vm;
        weft::install_script_platform(vm, weft::ScriptPlatformModule::WebHost);
        vm.eval(read_text_file(WEFT_SOURCE_DIR "/tests/fixtures/auto-layout/fixture.js"));
        std::ostringstream out;
        out << "{\"fixture\":\"auto-layout-v1\",\"frames\":[";
        for (int frame = 0; frame < weft::test_constants::layout::fixture_frames; frame++) {
            if (frame) {
                vm.eval("autoLayoutFixture.advance()");
            }
            if (frame) {
                out << ',';
            }
            out << '[';
            for (int i = 0; i < weft::test_constants::layout::auto_fixture_scenes; i++) {
                auto dom = vm.snapshot_subtree("#scene-" + std::to_string(i));
                const auto p = layout_document(dom, "", weft::constants::default_view_width,
                                               weft::constants::default_view_height, fonts)
                                   .page;
                if (i) {
                    out << ',';
                }
                out << '[';
                for (std::size_t j = 0; j < p.geometry.boxes.size(); j++) {
                    auto b = p.geometry.boxes[j];
                    if (j) {
                        out << ',';
                    }
                    out << "{\"id\":\"" << b.id << "\",\"x\":" << b.x << ",\"y\":" << b.y << ",\"width\":" << b.width
                        << ",\"height\":" << b.height << '}';
                }
                out << ']';
                if (i == 0) {
                    vm.collect_garbage();
                    require(p.paints.front().paint.text == "仓库 / depot 0", "GC invalidated value paint");
                }
            }
            out << ']';
        }
        out << "],\"passed\":true}";
        require(argc == 2, "report path");
        const std::filesystem::path path(argv[1]);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << out.str();
        std::cout
            << "PASS automatic layout fixture on the document formatter: 48 deterministic mutation scenes, intrinsic CJK, multiline ASCII, out-of-flow height\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
