#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <string>

// This fixture uses ASCII event identifiers. The application supplies its JSON
// encoder; these tests exercise recording lifetime, toggling and eviction.
std::string json_string(const std::string& value) {
    return '"' + value + '"';
}

#include "../probes/gallery_owner_trace.hpp"
#include "test_constants/script.hpp"

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "output directory required");
        const std::filesystem::path folder = argv[1];
        std::filesystem::create_directories(folder);
        const auto off = folder / "off.json";
        const auto ring = folder / "ring.json";
        const auto toggle = folder / "toggle.json";
        std::filesystem::remove(off);
        auto control = std::make_shared<GalleryProfileControl>();
        const auto stamp = std::chrono::steady_clock::now();
        {
            GalleryOwnerTrace trace(off, 1, control);
            trace.emit("disabled", stamp, 1);
            GalleryOwnerTrace::Scope scope(trace, "disabled scope", "{}");
        }
        require(!std::filesystem::exists(off), "disabled recorder performed file I/O");
        control->enabled = true;
        {
            GalleryOwnerTrace trace(ring, 1, control);
            for (unsigned i = 0; i < caseelephant::test_constants::script::trace_ring_overflow_events; ++i) {
                trace.emit("event" + std::to_string(i), stamp, double(i % 10));
            }
            control->enabled = false;
            trace.emit("disabled tail", stamp, 1);
        }
        const auto data = read(ring);
        require(data.starts_with("{\"traceEvents\":[{\"ph\":\"X\",\"pid\":1,\"tid\":1,\"name\":\"event4\""),
                "ring did not retain its latest events");
        require(data.find("\"name\":\"event65539\"") != data.npos && data.find("disabled tail") == data.npos &&
                    data.find("\"emitted_events\":65540") != data.npos &&
                    data.find("\"dropped_events\":4}") != data.npos,
                "ring eviction or disabled accounting differed");
        {
            GalleryOwnerTrace trace(toggle, 1, control);
            {
                GalleryOwnerTrace::Scope scope(trace, "disabled scope", "{}");
                control->enabled = true;
            }
            {
                GalleryOwnerTrace::Scope scope(trace, "operation began disabled", "{}", false);
            }
            trace.emit("input scene=7", stamp, 1);
            trace.emit("owner publication", stamp, 2);
            control->enabled = false;
            trace.emit("not sampled", stamp, 1);
        }
        const auto sampled = read(toggle);
        require(sampled.find("disabled scope") == sampled.npos &&
                    sampled.find("operation began disabled") == sampled.npos &&
                    sampled.find("not sampled") == sampled.npos &&
                    sampled.find("\"sampled_inputs\":1") != sampled.npos &&
                    sampled.find("\"sampled_publications\":1") != sampled.npos,
                "toggle fabricated a scope or discarded recorded events at close");
        std::cout
            << "PASS trace off/no I/O, latest bounded window, scope activation, toggle counters and close persistence\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
