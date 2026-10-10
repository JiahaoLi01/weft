#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

// File helpers shared by the behavior tests and the probes.
namespace caseelephant::testing {
    // Exact bytes of a file.
    inline std::string read_file(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Cannot read: " + path.string());
        }
        return {std::istreambuf_iterator<char>(input), {}};
    }

    // Text-mode read: the platform's line endings become '\n'. Use where a fixture may be checked out
    // with CRLF and the consumer expects '\n'.
    inline std::string read_text_file(const std::filesystem::path& path) {
        std::ifstream input(path);
        if (!input) {
            throw std::runtime_error("Cannot read: " + path.string());
        }
        return {std::istreambuf_iterator<char>(input), {}};
    }

    inline void write_file(const std::filesystem::path& path, std::string_view content) {
        std::ofstream output(path, std::ios::binary);
        if (!output || !output.write(content.data(), static_cast<std::streamsize>(content.size()))) {
            throw std::runtime_error("Cannot write: " + path.string());
        }
    }

#ifdef CE_SOURCE_DIR
    // Path of a checked-in file, relative to the source tree root (leading '/' optional).
    inline std::filesystem::path source_path(std::string_view relative) {
        while (!relative.empty() && (relative.front() == '/' || relative.front() == '\\')) {
            relative.remove_prefix(1);
        }
        return std::filesystem::path(CE_SOURCE_DIR) / std::filesystem::path(relative);
    }

    inline std::string read_source_file(std::string_view relative) {
        return read_file(source_path(relative));
    }

    inline std::string read_source_text_file(std::string_view relative) {
        return read_text_file(source_path(relative));
    }
#endif
} // namespace caseelephant::testing
