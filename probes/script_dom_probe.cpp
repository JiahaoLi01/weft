#include "script/script_dom.hpp"
#include "../tests/support/files.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
    using weft::testing::read_file;

    // Evidence text files end with a newline.
    void write_file(const std::filesystem::path& path, const std::string& content) {
        weft::testing::write_file(path, content + '\n');
    }
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3 || argc > 4) {
            throw std::runtime_error("Usage: script_dom_probe <d3.js> <d3_probe.js> [output-directory]");
        }
        weft::ScriptDom probe;
        probe.eval(read_file(argv[1]), argv[1]);
        const auto report = probe.eval(read_file(argv[2]), argv[2]);
        probe.collect_garbage();
        if (argc == 4) {
            const std::filesystem::path output(argv[3]);
            std::filesystem::create_directories(output);
            write_file(output / "d3-native-report.json", report);
            write_file(output / "d3-native-dom.json", probe.dump_document());
        }
        const auto stats = probe.stats();
        std::cout << report << '\n';
        std::cout << "Native DOM nodes: " << stats.live_nodes << ", created: " << stats.created_nodes
                  << ", finalized: " << stats.finalized_nodes << ", GC runs: " << stats.gc_runs << '\n';
        std::cout << "M0 probe: SVG DOM and generated path strings only; no layout or SVG rasterization.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
