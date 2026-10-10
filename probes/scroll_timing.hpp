#pragma once

#include "probe_common.hpp"
#include "probe_constants.hpp"

namespace probes {
    // Bounded benchmark samples. No average-only performance reporting.
    struct ScrollTimings {
        std::vector<std::array<double, weft::probe_constants::scroll_timing_columns>> rows;

        std::string json() const {
            std::ostringstream out;
            out << '{';
            bool first = true;
            const char* names[]{"owner_input_ms",    "layout_ms",          "packet_ms", "prepare_ms", "encode_ms",
                                "submit_present_ms", "completion_wait_ms", "gpu_ui_ms", "total_ms"};
            for (std::size_t i = 0; i < weft::probe_constants::scroll_timing_columns; ++i) {
                std::vector<double> values;
                for (const auto& r : rows) {
                    values.push_back(r[i]);
                }
                std::sort(values.begin(), values.end());
                require(!values.empty(), "performance samples required");
                auto percentile = [&](double p) {
                    return values[std::size_t(std::ceil(p * values.size())) - 1];
                };
                if (!first) {
                    out << ',';
                }
                first = false;
                out << json_string(names[i]) << ":{\"p50\":" << percentile(.5) << ",\"p95\":" << percentile(.95)
                    << ",\"p99\":" << percentile(.99) << ",\"max\":" << values.back() << '}';
            }
            out << '}';
            return out.str();
        }
    };
} // namespace probes
