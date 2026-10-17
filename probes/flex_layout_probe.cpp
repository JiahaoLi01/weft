#include "layout/flex.hpp"
#include "../tests/fixtures/flex_layout_cases.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace {
    std::string quoted(const std::string& input) {
        std::ostringstream stream;
        stream << std::quoted(input);
        return stream.str();
    }

    // Whether raw Yoga lays a box tree out: its root and every box with children explicitly flex (FlexMode::RawYoga).
    bool raw_profile(const weft::FlexNode& n, bool root = true) {
        const auto display = n.style.text(weft::PropertyId::Display);
        if ((root || !n.children.empty()) && display != "flex" && display != "inline-flex") {
            return false;
        }
        return std::all_of(n.children.begin(), n.children.end(), [](const weft::FlexNode& c) {
            return raw_profile(c, false);
        });
    }

    std::string report(weft::Font& font, weft::FlexMode mode) {
        std::ostringstream out;
        out << std::setprecision(std::numeric_limits<float>::max_digits10); // Round-trips every float box value.
        out << "{\"fixture\":\"caseelephant-flex-v1\",\"input_sha256\":\"" << weft::fixtures::input_sha256
            << "\",\"backend\":\"yoga-3.2.1+ce-definite-basis-1\",\"mode\":\""
            << (mode == weft::FlexMode::RawYoga ? "raw-yoga" : "css-adapter") << "\",\"cases\":[";
        bool comma{};
        for (const auto& test : weft::fixtures::flex_cases()) {
            if (mode == weft::FlexMode::RawYoga && !raw_profile(test.root)) {
                continue;
            }
            if (comma) {
                out << ',';
            }
            comma = true;
            out << "{\"id\":" << quoted(test.id) << ",\"boxes\":[";
            bool box_comma{};
            for (const auto& b : weft::run_flex(test.root, font, mode)) {
                if (box_comma) {
                    out << ',';
                }
                box_comma = true;
                out << "{\"id\":" << quoted(b.id) << ",\"x\":" << b.x << ",\"y\":" << b.y << ",\"width\":" << b.width
                    << ",\"height\":" << b.height << ",\"text_width\":" << b.text_width
                    << ",\"text_height\":" << b.text_height << ",\"text_lines\":" << b.text_lines
                    << ",\"baseline\":" << b.baseline << ",\"measure_calls\":" << b.measure_calls << '}';
            }
            out << "]}";
        }
        out << "]}\n";
        return out.str();
    }
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            throw std::runtime_error("Usage: flex_layout_probe <font-path> <output-directory>");
        }
        weft::Font font(argv[1]);
        std::filesystem::create_directories(argv[2]);
        for (const auto mode : {weft::FlexMode::RawYoga, weft::FlexMode::CssAdapter}) {
            const auto name = mode == weft::FlexMode::RawYoga ? "raw-yoga.json" : "css-adapter.json";
            std::ofstream file(std::filesystem::path(argv[2]) / name, std::ios::binary);
            const auto content = report(font, mode);
            if (!file || !(file << content)) {
                throw std::runtime_error("Cannot write Flex report");
            }
        }
        std::cout
            << weft::fixtures::flex_cases().size()
            << " shared Flex and grid cases written (Yoga + ce1, with/without CSS adapter; raw Yoga only where every container is flex). No "
               "DOM or renderer integration.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
