#include "caseelephant/flex_probe.hpp"
#include "../tests/fixtures/flex_layout_cases.hpp"
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

    std::string report(caseelephant::TextProbeFont& font, caseelephant::FlexProbeMode mode) {
        std::ostringstream out;
        out << std::setprecision(std::numeric_limits<float>::max_digits10); // Round-trips every float box value.
        out << "{\"fixture\":\"caseelephant-flex-v1\",\"input_sha256\":\"" << caseelephant::fixtures::input_sha256
            << "\",\"backend\":\"yoga-3.2.1+ce-definite-basis-1\",\"mode\":\""
            << (mode == caseelephant::FlexProbeMode::RawYoga ? "raw-yoga" : "css-adapter") << "\",\"cases\":[";
        bool comma{};
        for (const auto& test : caseelephant::fixtures::flex_cases()) {
            if (comma) {
                out << ',';
            }
            comma = true;
            out << "{\"id\":" << quoted(test.id) << ",\"boxes\":[";
            bool box_comma{};
            for (const auto& b : caseelephant::run_flex_probe(test.root, font, mode)) {
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
        caseelephant::TextProbeFont font(argv[1]);
        std::filesystem::create_directories(argv[2]);
        for (const auto mode : {caseelephant::FlexProbeMode::RawYoga, caseelephant::FlexProbeMode::CssAdapter}) {
            const auto name = mode == caseelephant::FlexProbeMode::RawYoga ? "raw-yoga.json" : "css-adapter.json";
            std::ofstream file(std::filesystem::path(argv[2]) / name, std::ios::binary);
            const auto content = report(font, mode);
            if (!file || !(file << content)) {
                throw std::runtime_error("Cannot write Flex report");
            }
        }
        std::cout
            << caseelephant::fixtures::flex_cases().size()
            << " shared Flex cases written (Yoga + ce1, with/without CSS adapter). No DOM or renderer integration.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
